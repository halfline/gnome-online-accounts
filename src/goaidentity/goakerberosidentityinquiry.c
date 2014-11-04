/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Ray Strode
 */

#include "config.h"

#include "goakerberosidentityinquiry.h"
#include "goaidentityinquiryprivate.h"
#include "goalogging.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

struct _GoaKerberosIdentityInquiryPrivate
{
  GoaIdentity *identity;
  char *name;
  char *banner;
  GList *queries;
  int number_of_queries;
  int number_of_unanswered_queries;
};

typedef struct
{
  GoaIdentityInquiry *inquiry;
  krb5_prompt *kerberos_prompt;
  gboolean is_answered;
} GoaKerberosIdentityQuery;

static void identity_inquiry_interface_init (GoaIdentityInquiryInterface *
                                             interface);
static void initable_interface_init (GInitableIface *interface);

G_DEFINE_TYPE_WITH_CODE (GoaKerberosIdentityInquiry,
                         goa_kerberos_identity_inquiry,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                initable_interface_init)
                         G_IMPLEMENT_INTERFACE (GOA_TYPE_IDENTITY_INQUIRY,
                                                identity_inquiry_interface_init));

static gboolean
goa_kerberos_identity_inquiry_initable_init (GInitable * initable,
                                             GCancellable *cancellable,
                                             GError ** error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      return FALSE;
    }

  return TRUE;
}

static void
initable_interface_init (GInitableIface *interface)
{
  interface->init = goa_kerberos_identity_inquiry_initable_init;
}

static GoaKerberosIdentityQuery *
goa_kerberos_identity_query_new (GoaIdentityInquiry * inquiry,
                                 krb5_prompt * kerberos_prompt)
{
  GoaKerberosIdentityQuery *query;

  query = g_slice_new (GoaKerberosIdentityQuery);
  query->inquiry = inquiry;
  query->kerberos_prompt = kerberos_prompt;
  query->is_answered = FALSE;

  return query;
}

static void
goa_kerberos_identity_query_free (GoaKerberosIdentityQuery *query)
{
  g_slice_free (GoaKerberosIdentityQuery, query);
}

static void
goa_kerberos_identity_inquiry_dispose (GObject *object)
{
  GoaKerberosIdentityInquiry *self = GOA_KERBEROS_IDENTITY_INQUIRY (object);

  g_clear_object (&self->priv->identity);
  g_clear_pointer (&self->priv->name, (GDestroyNotify) g_free);
  g_clear_pointer (&self->priv->banner, (GDestroyNotify) g_free);

  g_list_foreach (self->priv->queries,
                  (GFunc) goa_kerberos_identity_query_free, NULL);
  g_clear_pointer (&self->priv->queries, (GDestroyNotify) g_list_free);
}

static void
goa_kerberos_identity_inquiry_finalize (GObject *object)
{
  G_OBJECT_CLASS (goa_kerberos_identity_inquiry_parent_class)->finalize (object);
}

static void
goa_kerberos_identity_inquiry_class_init (GoaKerberosIdentityInquiryClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = goa_kerberos_identity_inquiry_dispose;
  object_class->finalize = goa_kerberos_identity_inquiry_finalize;

  g_type_class_add_private (klass, sizeof (GoaKerberosIdentityInquiryPrivate));
}

static void
goa_kerberos_identity_inquiry_init (GoaKerberosIdentityInquiry *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                            GOA_TYPE_KERBEROS_IDENTITY_INQUIRY,
                                            GoaKerberosIdentityInquiryPrivate);
}

GoaIdentityInquiry *
goa_kerberos_identity_inquiry_new (GoaKerberosIdentity * identity,
                                   const char *name,
                                   const char *banner,
                                   krb5_prompt prompts[], int number_of_prompts)
{
  GObject *object;
  GoaIdentityInquiry *inquiry;
  GoaKerberosIdentityInquiry *self;
  GError *error;
  int i;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY (identity), NULL);
  g_return_val_if_fail (number_of_prompts > 0, NULL);

  object = g_object_new (GOA_TYPE_KERBEROS_IDENTITY_INQUIRY, NULL);

  inquiry = GOA_IDENTITY_INQUIRY (object);
  self = GOA_KERBEROS_IDENTITY_INQUIRY (object);

  /* FIXME: make these construct properties */
  self->priv->identity = g_object_ref (identity);
  self->priv->name = g_strdup (name);
  self->priv->banner = g_strdup (banner);

  self->priv->number_of_queries = 0;
  for (i = 0; i < number_of_prompts; i++)
    {
      GoaKerberosIdentityQuery *query;

      query = goa_kerberos_identity_query_new (inquiry, &prompts[i]);

      self->priv->queries = g_list_prepend (self->priv->queries, query);
      self->priv->number_of_queries++;
    }
  self->priv->queries = g_list_reverse (self->priv->queries);

  self->priv->number_of_unanswered_queries = self->priv->number_of_queries;

  error = NULL;
  if (!g_initable_init (G_INITABLE (self), NULL, &error))
    {
      goa_debug ("%s", error->message);
      g_error_free (error);
      g_object_unref (self);
      return NULL;
    }

  return inquiry;
}

static GoaIdentity *
goa_kerberos_identity_inquiry_get_identity (GoaIdentityInquiry *inquiry)
{
  GoaKerberosIdentityInquiry *self;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry), NULL);

  self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  return self->priv->identity;
}

static char *
goa_kerberos_identity_inquiry_get_name (GoaIdentityInquiry *inquiry)
{
  GoaKerberosIdentityInquiry *self;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry), NULL);

  self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  return g_strdup (self->priv->name);
}

static char *
goa_kerberos_identity_inquiry_get_banner (GoaIdentityInquiry *inquiry)
{
  GoaKerberosIdentityInquiry *self;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry), NULL);

  self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  return g_strdup (self->priv->banner);
}

static gboolean
goa_kerberos_identity_inquiry_is_complete (GoaIdentityInquiry *inquiry)
{
  GoaKerberosIdentityInquiry *self;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry), FALSE);

  self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  return self->priv->number_of_unanswered_queries == 0;
}

static void
goa_kerberos_identity_inquiry_mark_query_answered (GoaKerberosIdentityInquiry * self,
                                                   GoaKerberosIdentityQuery * query)
{
  if (query->is_answered)
    {
      return;
    }

  query->is_answered = TRUE;
  self->priv->number_of_unanswered_queries--;

  if (self->priv->number_of_unanswered_queries == 0)
    {
      _goa_identity_inquiry_emit_complete (GOA_IDENTITY_INQUIRY (self));
    }
}

static void
goa_kerberos_identity_inquiry_answer_query (GoaIdentityInquiry * inquiry,
                                            GoaIdentityQuery *query,
                                            const char *answer)
{
  GoaKerberosIdentityInquiry *self;
  GoaKerberosIdentityQuery *kerberos_query = (GoaKerberosIdentityQuery *) query;

  g_return_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry));
  g_return_if_fail (inquiry == kerberos_query->inquiry);
  g_return_if_fail (!goa_kerberos_identity_inquiry_is_complete (inquiry));

  self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  strncpy (kerberos_query->kerberos_prompt->reply->data,
           answer, kerberos_query->kerberos_prompt->reply->length);
  kerberos_query->kerberos_prompt->reply->length =
    (unsigned int) strlen (kerberos_query->kerberos_prompt->reply->data);

  goa_kerberos_identity_inquiry_mark_query_answered (self, kerberos_query);
}

static void
goa_kerberos_identity_inquiry_iter_init (GoaIdentityInquiryIter * iter,
                                         GoaIdentityInquiry * inquiry)
{
  GoaKerberosIdentityInquiry *self = GOA_KERBEROS_IDENTITY_INQUIRY (inquiry);

  iter->data = self->priv->queries;
}

static GoaIdentityQuery *
goa_kerberos_identity_inquiry_iter_next (GoaIdentityInquiryIter * iter,
                                         GoaIdentityInquiry * inquiry)
{
  GoaIdentityQuery *query;
  GList *node;

  node = iter->data;

  if (node == NULL)
    {
      return NULL;
    }

  query = (GoaIdentityQuery *) node->data;

  node = node->next;

  iter->data = node;

  return query;
}

static GoaIdentityQueryMode
goa_kerberos_identity_query_get_mode (GoaIdentityInquiry * inquiry,
                                      GoaIdentityQuery * query)
{
  GoaKerberosIdentityQuery *kerberos_query = (GoaKerberosIdentityQuery *) query;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry),
                        GOA_KERBEROS_IDENTITY_QUERY_MODE_INVISIBLE);
  g_return_val_if_fail (inquiry == kerberos_query->inquiry,
                        GOA_KERBEROS_IDENTITY_QUERY_MODE_INVISIBLE);

  if (kerberos_query->kerberos_prompt->hidden)
    {
      return GOA_KERBEROS_IDENTITY_QUERY_MODE_INVISIBLE;
    }
  else
    {
      return GOA_KERBEROS_IDENTITY_QUERY_MODE_VISIBLE;
    }
}

static char *
goa_kerberos_identity_query_get_prompt (GoaIdentityInquiry * inquiry,
                                        GoaIdentityQuery * query)
{
  GoaKerberosIdentityQuery *kerberos_query = (GoaKerberosIdentityQuery *) query;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry),
                        GOA_KERBEROS_IDENTITY_QUERY_MODE_INVISIBLE);
  g_return_val_if_fail (inquiry == kerberos_query->inquiry, NULL);

  return g_strdup (kerberos_query->kerberos_prompt->prompt);
}

static gboolean
goa_kerberos_identity_query_is_answered (GoaIdentityInquiry * inquiry,
                                         GoaIdentityQuery * query)
{
  GoaKerberosIdentityQuery *kerberos_query = (GoaKerberosIdentityQuery *) query;

  g_return_val_if_fail (GOA_IS_KERBEROS_IDENTITY_INQUIRY (inquiry),
                        GOA_KERBEROS_IDENTITY_QUERY_MODE_INVISIBLE);
  g_return_val_if_fail (inquiry == kerberos_query->inquiry, FALSE);

  return kerberos_query->is_answered;
}

static void
identity_inquiry_interface_init (GoaIdentityInquiryInterface *interface)
{
  interface->get_identity = goa_kerberos_identity_inquiry_get_identity;
  interface->get_name = goa_kerberos_identity_inquiry_get_name;
  interface->get_banner = goa_kerberos_identity_inquiry_get_banner;
  interface->is_complete = goa_kerberos_identity_inquiry_is_complete;
  interface->answer_query = goa_kerberos_identity_inquiry_answer_query;
  interface->iter_init = goa_kerberos_identity_inquiry_iter_init;
  interface->iter_next = goa_kerberos_identity_inquiry_iter_next;
  interface->get_mode = goa_kerberos_identity_query_get_mode;
  interface->get_prompt = goa_kerberos_identity_query_get_prompt;
  interface->is_answered = goa_kerberos_identity_query_is_answered;
}
