/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012, 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Ray Strode
 * Based on work by Colin Walters
 */

#include "config.h"

#include "goaalarm.h"

#ifdef HAVE_TIMERFD
#include <sys/timerfd.h>
#endif

#include <unistd.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include "goalogging.h"

#define MAX_TIMEOUT_INTERVAL (10 *1000)

typedef enum
{
  GOA_ALARM_TYPE_UNSCHEDULED,
  GOA_ALARM_TYPE_TIMER,
  GOA_ALARM_TYPE_TIMEOUT,
} GoaAlarmType;

struct _GoaAlarmPrivate
{
  GCancellable *cancellable;
  gulong cancelled_id;
  GDateTime *time;
  GDateTime *previous_wakeup_time;
  GMainContext *context;
  GSource *immediate_wakeup_source;
  GRecMutex lock;

  GoaAlarmType type;
  GSource *scheduled_wakeup_source;
  GInputStream *stream; /* NULL, unless using timerfd */
};

enum
{
  FIRED,
  REARMED,
  NUMBER_OF_SIGNALS,
};

enum
{
  PROP_0,
  PROP_TIME
};

static void schedule_wakeups (GoaAlarm *self);
static void schedule_wakeups_with_timeout_source (GoaAlarm *self);
static guint signals[NUMBER_OF_SIGNALS] = { 0 };

G_DEFINE_TYPE (GoaAlarm, goa_alarm, G_TYPE_OBJECT);

static void
clear_scheduled_immediate_wakeup (GoaAlarm *self)
{
  g_clear_pointer (&self->priv->immediate_wakeup_source,
                   (GDestroyNotify) g_source_destroy);
}

static void
clear_scheduled_timer_wakeups (GoaAlarm *self)
{
#ifdef HAVE_TIMERFD
  GError *error;
  gboolean is_closed;

  g_clear_pointer (&self->priv->scheduled_wakeup_source, (GDestroyNotify) g_source_destroy);

  error = NULL;
  is_closed = g_input_stream_close (self->priv->stream, NULL, &error);

  if (!is_closed)
    {
      goa_warning ("GoaAlarm: could not close timer stream: %s", error->message);
      g_error_free (error);
    }

  g_clear_object (&self->priv->stream);
#endif
}

static void
clear_scheduled_timeout_wakeups (GoaAlarm *self)
{
  g_clear_pointer (&self->priv->scheduled_wakeup_source, (GDestroyNotify) g_source_destroy);
}

static void
clear_scheduled_wakeups (GoaAlarm *self)
{
  g_rec_mutex_lock (&self->priv->lock);
  clear_scheduled_immediate_wakeup (self);

  switch (self->priv->type)
    {
    case GOA_ALARM_TYPE_TIMER:
      clear_scheduled_timer_wakeups (self);
      break;

    case GOA_ALARM_TYPE_TIMEOUT:
      clear_scheduled_timeout_wakeups (self);
      break;

    default:
      break;
    }

  g_clear_pointer (&self->priv->previous_wakeup_time,
                   (GDestroyNotify) g_date_time_unref);

  self->priv->type = GOA_ALARM_TYPE_UNSCHEDULED;
  g_rec_mutex_unlock (&self->priv->lock);
}

static void
goa_alarm_dispose (GObject *object)
{
  GoaAlarm *self = GOA_ALARM (object);

  g_clear_object (&self->priv->cancellable);
  g_clear_pointer (&self->priv->context, (GDestroyNotify) g_main_context_unref);
  g_clear_pointer (&self->priv->time, (GDestroyNotify) g_date_time_unref);

  G_OBJECT_CLASS (goa_alarm_parent_class)->dispose (object);
}

static void
goa_alarm_finalize (GObject *object)
{
  GoaAlarm *self = GOA_ALARM (object);

  clear_scheduled_wakeups (self);

  g_rec_mutex_clear (&self->priv->lock);

  G_OBJECT_CLASS (goa_alarm_parent_class)->finalize (object);
}

static void
goa_alarm_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *param_spec)
{
  GoaAlarm *self = GOA_ALARM (object);
  GDateTime *time;

  switch (property_id)
    {
    case PROP_TIME:
      time = (GDateTime *) g_value_get_boxed (value);
      goa_alarm_set_time (self, time, self->priv->cancellable);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, param_spec);
      break;
    }
}

static void
goa_alarm_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *param_spec)
{
  GoaAlarm *self = GOA_ALARM (object);

  switch (property_id)
    {
    case PROP_TIME:
      g_value_set_boxed (value, self->priv->time);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, param_spec);
      break;
    }
}

static void
goa_alarm_class_init (GoaAlarmClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = goa_alarm_dispose;
  object_class->finalize = goa_alarm_finalize;
  object_class->get_property = goa_alarm_get_property;
  object_class->set_property = goa_alarm_set_property;

  g_type_class_add_private (klass, sizeof (GoaAlarmPrivate));

  signals[FIRED] = g_signal_new ("fired",
                                 G_TYPE_FROM_CLASS (klass),
                                 G_SIGNAL_RUN_LAST,
                                 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[REARMED] = g_signal_new ("rearmed",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  g_object_class_install_property (object_class,
                                   PROP_TIME,
                                   g_param_spec_boxed ("time",
                                                       _("Time"),
                                                       _("Time to fire"),
                                                       G_TYPE_DATE_TIME,
                                                       G_PARAM_READWRITE));
}

static void
goa_alarm_init (GoaAlarm *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, GOA_TYPE_ALARM, GoaAlarmPrivate);
  self->priv->type = GOA_ALARM_TYPE_UNSCHEDULED;
  g_rec_mutex_init (&self->priv->lock);
}

static gboolean
async_alarm_cancel_idle_cb (gpointer user_data)
{
  GoaAlarm *self = user_data;

  clear_scheduled_wakeups (self);
  return G_SOURCE_REMOVE;
}

static void
on_cancelled (GCancellable *cancellable, gpointer user_data)
{
  GoaAlarm *self = GOA_ALARM (user_data);
  GSource *idle_source;


  idle_source = g_idle_source_new ();
  g_source_set_priority (idle_source, G_PRIORITY_HIGH_IDLE);
  g_source_set_callback (idle_source, async_alarm_cancel_idle_cb, g_object_ref (self), g_object_unref);
  g_source_attach (idle_source, self->priv->context);
  g_source_unref (idle_source);
}

static void
fire_alarm (GoaAlarm *self)
{
  g_signal_emit (G_OBJECT (self), signals[FIRED], 0);
}

static void
rearm_alarm (GoaAlarm *self)
{
  g_signal_emit (G_OBJECT (self), signals[REARMED], 0);
}

static void
fire_or_rearm_alarm (GoaAlarm *self)
{
  GTimeSpan time_until_fire;
  GTimeSpan previous_time_until_fire;
  GDateTime *now;

  now = g_date_time_new_now_local ();
  time_until_fire = g_date_time_difference (self->priv->time, now);

  if (self->priv->previous_wakeup_time == NULL)
    {
      self->priv->previous_wakeup_time = now;

      /* If, according to the time, we're past when we should have fired,
       * then fire the alarm.
       */
      if (time_until_fire <= 0)
        fire_alarm (self);
    }
  else
    {
      previous_time_until_fire =
          g_date_time_difference (self->priv->time,
                                  self->priv->previous_wakeup_time);

      g_date_time_unref (self->priv->previous_wakeup_time);
      self->priv->previous_wakeup_time = now;

      /* If, according to the time, we're past when we should have fired,
       * and this is the first wakeup where that's been true then fire
       * the alarm. The first check makes sure we don't fire prematurely,
       * and the second check makes sure we don't fire more than once
       */
      if (time_until_fire <= 0 && previous_time_until_fire > 0)
        {
          fire_alarm (self);

          /* If, according to the time, we're before when we should fire,
           * and we previously fired the alarm, then we've jumped back in
           * time and need to rearm the alarm.
           */
        }
      else if (time_until_fire > 0 && previous_time_until_fire <= 0)
        {
          rearm_alarm (self);
        }
    }
}

static gboolean
on_immediate_wakeup_source_ready (GoaAlarm *self)
{
  g_return_val_if_fail (self->priv->type != GOA_ALARM_TYPE_UNSCHEDULED, FALSE);

  g_rec_mutex_lock (&self->priv->lock);
  if (g_cancellable_is_cancelled (self->priv->cancellable))
    goto out;

  fire_or_rearm_alarm (self);

out:
  g_rec_mutex_unlock (&self->priv->lock);
  return FALSE;
}

#ifdef HAVE_TIMERFD
static gboolean
on_timer_source_ready (GObject *stream, GTask *task)
{
  gint64 number_of_fires;
  gssize bytes_read;
  gboolean run_again = FALSE;
  GError *error = NULL;
  GoaAlarm *self;
  GCancellable *cancellable;

  self = g_task_get_source_object (task);
  cancellable = g_task_get_cancellable (task);

  g_return_val_if_fail (GOA_IS_ALARM (self), FALSE);

  g_rec_mutex_lock (&self->priv->lock);

  if (self->priv->type == GOA_ALARM_TYPE_UNSCHEDULED)
    {
      goa_debug ("GoaAlarm: timer source was unscheduled after "
                 "callback was invoked, but before callback got "
                 "the lock.");
      goto out;
    }
  else if (self->priv->type != GOA_ALARM_TYPE_TIMER)
    {
      goa_warning ("GoaAlarm: timer source ready callback called "
                   "when timer source isn't supposed to be used. "
                   "Current timer type is %u", self->priv->type);
      goto out;
    }

  if (g_cancellable_is_cancelled (cancellable))
    goto out;

  bytes_read =
    g_pollable_input_stream_read_nonblocking (G_POLLABLE_INPUT_STREAM (stream),
                                              &number_of_fires, sizeof (gint64),
                                              NULL, &error);

  if (bytes_read < 0)
    {
      goa_warning ("GoaAlarm: failed to read from timer fd: %s\n",
                   error->message);
      g_error_free (error);
      goto out;
    }

  if (bytes_read == sizeof (gint64))
    {
      if (number_of_fires < 0 || number_of_fires > 1)
        {
          goa_warning ("GoaAlarm: expected timerfd to report firing once,"
                       "but it reported firing %ld times\n", (long) number_of_fires);
        }
    }

  fire_or_rearm_alarm (self);
  run_again = TRUE;
out:
  g_rec_mutex_unlock (&self->priv->lock);
  return run_again;
}

static void
clear_timer_source (GTask *task)
{
  GoaAlarm *self;

  self = g_task_get_source_object (task);
  self->priv->scheduled_wakeup_source = NULL;

  g_object_unref (task);
}
#endif

static gboolean
schedule_wakeups_with_timerfd (GoaAlarm *self)
{
#ifdef HAVE_TIMERFD
  struct itimerspec timer_spec;
  int fd;
  int result;
  GSource *source;
  GTask *task;
  static gboolean seen_before = FALSE;

  if (!seen_before)
    {
      goa_debug ("GoaAlarm: trying to use kernel timer");
      seen_before = TRUE;
    }

  fd = timerfd_create (CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);

  if (fd < 0)
    {
      goa_debug ("GoaAlarm: could not create timer fd: %m");
      return FALSE;
    }

  memset (&timer_spec, 0, sizeof (timer_spec));
  timer_spec.it_value.tv_sec = g_date_time_to_unix (self->priv->time) + 1;

  result = timerfd_settime (fd,
                            TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                            &timer_spec, NULL);

  if (result < 0)
    {
      goa_debug ("GoaAlarm: could not set timer: %m");
      return FALSE;
    }

  self->priv->type = GOA_ALARM_TYPE_TIMER;
  self->priv->stream = g_unix_input_stream_new (fd, TRUE);

  task = g_task_new (self, self->priv->cancellable, NULL, NULL);

  source =
    g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM
                                           (self->priv->stream),
                                           self->priv->cancellable);
  self->priv->scheduled_wakeup_source = source;
  g_source_set_callback (self->priv->scheduled_wakeup_source,
                         (GSourceFunc) on_timer_source_ready, task,
                         (GDestroyNotify) clear_timer_source);
  g_source_attach (self->priv->scheduled_wakeup_source, self->priv->context);
  g_source_unref (source);

  return TRUE;

#endif /*HAVE_TIMERFD */

  return FALSE;
}

static gboolean
on_timeout_source_ready (GoaAlarm *self)
{
  g_return_val_if_fail (GOA_IS_ALARM (self), FALSE);

  g_rec_mutex_lock (&self->priv->lock);

  if (g_cancellable_is_cancelled (self->priv->cancellable) ||
      self->priv->type == GOA_ALARM_TYPE_UNSCHEDULED)
    goto out;

  fire_or_rearm_alarm (self);

  if (g_cancellable_is_cancelled (self->priv->cancellable))
    goto out;

  schedule_wakeups_with_timeout_source (self);

out:
  g_rec_mutex_unlock (&self->priv->lock);
  return FALSE;
}

static void
clear_timeout_source_pointer (GoaAlarm *self)
{
  self->priv->scheduled_wakeup_source = NULL;
}

static void
schedule_wakeups_with_timeout_source (GoaAlarm *self)
{
  GDateTime *now;
  GSource   *source;
  GTimeSpan  time_span;
  guint      interval;

  self->priv->type = GOA_ALARM_TYPE_TIMEOUT;

  now = g_date_time_new_now_local ();
  time_span = g_date_time_difference (self->priv->time, now);
  g_date_time_unref (now);

  time_span =
    CLAMP (time_span, 1000 *G_TIME_SPAN_MILLISECOND,
           G_MAXUINT *G_TIME_SPAN_MILLISECOND);
  interval = (guint) time_span / G_TIME_SPAN_MILLISECOND;

  /* We poll every 10 seconds or so because we want to catch time skew
   */
  interval = MIN (interval, MAX_TIMEOUT_INTERVAL);

  source = g_timeout_source_new (interval);

  self->priv->scheduled_wakeup_source = source;
  g_source_set_callback (self->priv->scheduled_wakeup_source,
                         (GSourceFunc)
                         on_timeout_source_ready,
                         self, (GDestroyNotify) clear_timeout_source_pointer);

  g_source_attach (self->priv->scheduled_wakeup_source, self->priv->context);
  g_source_unref (source);
}

static void
schedule_wakeups (GoaAlarm *self)
{
  gboolean wakeup_scheduled;

  wakeup_scheduled = schedule_wakeups_with_timerfd (self);

  if (!wakeup_scheduled)
    {
      static gboolean seen_before = FALSE;

      if (!seen_before)
        {
          goa_debug ("GoaAlarm: falling back to polling timeout");
          seen_before = TRUE;
        }
      schedule_wakeups_with_timeout_source (self);
    }
}

static void
clear_immediate_wakeup_source_pointer (GoaAlarm *self)
{
  self->priv->immediate_wakeup_source = NULL;
}

static void
schedule_immediate_wakeup (GoaAlarm *self)
{
  GSource *source;

  source = g_idle_source_new ();

  self->priv->immediate_wakeup_source = source;
  g_source_set_callback (self->priv->immediate_wakeup_source,
                         (GSourceFunc)
                         on_immediate_wakeup_source_ready,
                         self,
                         (GDestroyNotify) clear_immediate_wakeup_source_pointer);

  g_source_attach (self->priv->immediate_wakeup_source, self->priv->context);
  g_source_unref (source);
}

void
goa_alarm_set_time (GoaAlarm *self, GDateTime *time, GCancellable *cancellable)
{
  if (g_cancellable_is_cancelled (cancellable))
    return;

  g_rec_mutex_lock (&self->priv->lock);
  if (self->priv->cancellable != NULL && self->priv->cancellable != cancellable)
    g_cancellable_cancel (self->priv->cancellable);

  if (cancellable != NULL)
    g_object_ref (cancellable);

  if (self->priv->cancelled_id != 0)
    g_cancellable_disconnect (self->priv->cancellable, self->priv->cancelled_id);

  g_clear_object (&self->priv->cancellable);

  if (cancellable != NULL)
    self->priv->cancellable = cancellable;
  else
    self->priv->cancellable = g_cancellable_new ();

  self->priv->cancelled_id = g_cancellable_connect (self->priv->cancellable,
                                                    G_CALLBACK (on_cancelled),
                                                    self, NULL);

  g_date_time_ref (time);

  if (self->priv->time != NULL)
    g_date_time_unref (self->priv->time);

  self->priv->time = time;

  if (self->priv->context == NULL)
    self->priv->context = g_main_context_ref (g_main_context_default ());

  schedule_wakeups (self);

  /* Wake up right away, in case it's already expired leaving the gate */
  schedule_immediate_wakeup (self);
  g_rec_mutex_unlock (&self->priv->lock);
  g_object_notify (G_OBJECT (self), "time");
}

GDateTime *
goa_alarm_get_time (GoaAlarm *self)
{
  return self->priv->time;
}

GoaAlarm *
goa_alarm_new (void)
{
  GoaAlarm *self;

  self = GOA_ALARM (g_object_new (GOA_TYPE_ALARM, NULL));

  return GOA_ALARM (self);
}
