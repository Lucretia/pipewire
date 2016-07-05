/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <sys/socket.h>
#include <string.h>

#include <gst/gst.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixfdmessage.h>


#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/client/port.h"
#include "pinos/client/node.h"

#define PINOS_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_PORT, PinosPortPrivate))

#if 0
#define PINOS_DEBUG_TRANSPORT(format,args...) g_debug(format,##args)
#else
#define PINOS_DEBUG_TRANSPORT(format,args...)
#endif

#define MAX_BUFFER_SIZE 1024
#define MAX_FDS		16

struct _PinosPortPrivate
{
  PinosNode *node;

  gchar *name;
  GSocket *sockets[2];
  PinosDirection direction;
  GBytes *possible_formats;
  GBytes *format;
  PinosProperties *properties;

  int fd;
  GSource *socket_source;

  PinosBuffer recv_buffer;

  guint8 recv_data[MAX_BUFFER_SIZE];
  int recv_fds[MAX_FDS];

  guint8 send_data[MAX_BUFFER_SIZE];
  int send_fds[MAX_FDS];

  PinosBuffer *buffer;
  GPtrArray *peers;
  gchar **peer_paths;
  guint max_peers;

  PinosReceivedBufferCallback received_buffer_cb;
  gpointer received_buffer_data;
  GDestroyNotify received_buffer_notify;
};

G_DEFINE_ABSTRACT_TYPE (PinosPort, pinos_port, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_NODE,
  PROP_SOCKET,
  PROP_MAIN_CONTEXT,
  PROP_NAME,
  PROP_DIRECTION,
  PROP_MAX_PEERS,
  PROP_PEERS,
  PROP_POSSIBLE_FORMATS,
  PROP_FORMAT,
  PROP_PROPERTIES,
};

enum
{
  SIGNAL_FORMAT_REQUEST,
  SIGNAL_REMOVE,
  SIGNAL_LINKED,
  SIGNAL_UNLINKED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

void
pinos_port_set_received_buffer_cb (PinosPort *port,
                                   PinosReceivedBufferCallback cb,
                                   gpointer user_data,
                                   GDestroyNotify notify)
{
  PinosPortPrivate *priv;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;

  g_debug ("port %p: set callback", port);

  if (priv->received_buffer_notify)
    priv->received_buffer_notify (priv->received_buffer_data);;
  priv->received_buffer_cb = cb;
  priv->received_buffer_data = user_data;
  priv->received_buffer_notify = notify;
}

/**
 * pinos_port_remove:
 * @port: a #PinosPort
 *
 * Trigger removal of @port
 */
void
pinos_port_remove (PinosPort *port)
{
  g_return_if_fail (PINOS_IS_PORT (port));

  g_debug ("port %p: remove", port);
  g_signal_emit (port, signals[SIGNAL_REMOVE], 0, NULL);
}

/**
 * pinos_port_get_node:
 * @port: a #PinosPort
 *
 * Get the parent #PinosNode of @port
 *
 * Returns: the parent node or %NULL
 */
PinosNode *
pinos_port_get_node (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->node;
}

/**
 * pinos_port_get_socket:
 * @port: a #PinosPort
 *
 * Get the socket of @port
 *
 * Returns: the socket or %NULL
 */
GSocket *
pinos_port_get_socket (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->sockets[0];
}

/**
 * pinos_port_get_name:
 * @port: a #PinosPort
 *
 * Get the name of @port
 *
 * Returns: the name or %NULL
 */
const gchar *
pinos_port_get_name (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->name;
}

/**
 * pinos_port_get_direction:
 * @port: a #PinosPort
 *
 * Get the direction of @port
 *
 * Returns: the direction or %NULL
 */
PinosDirection
pinos_port_get_direction (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), PINOS_DIRECTION_INVALID);
  priv = port->priv;

  return priv->direction;
}

/**
 * pinos_port_get_possible_formats:
 * @port: a #PinosPort
 *
 * Get the possible formats of @port
 *
 * Returns: the possible formats or %NULL
 */
GBytes *
pinos_port_get_possible_formats (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  g_signal_emit (port, signals[SIGNAL_FORMAT_REQUEST], 0, NULL);
  return priv->possible_formats;
}

/**
 * pinos_port_get_format:
 * @port: a #PinosPort
 *
 * Get the format of @port
 *
 * Returns: the format or %NULL
 */
GBytes *
pinos_port_get_format (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->format;
}

/**
 * pinos_port_get_properties:
 * @port: a #PinosPort
 *
 * Get the properties of @port
 *
 * Returns: the properties or %NULL
 */
PinosProperties *
pinos_port_get_properties (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->properties;
}

/**
 * pinos_port_filter_formats:
 * @port: a #PinosPort
 * @filter: a #GBytes
 * @error: a #GError or %NULL
 *
 * Get all the currently supported formats for @port and filter the
 * results with @filter.
 *
 * Returns: the list of supported format. If %NULL is returned, @error will
 * be set.
 */
GBytes *
pinos_port_filter_formats (PinosPort  *port,
                           GBytes     *filter,
                           GError    **error)
{
  GstCaps *tmp, *caps, *cfilter;
  gchar *str;
  PinosPortPrivate *priv;
  GBytes *res;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  if (filter) {
    cfilter = gst_caps_from_string (g_bytes_get_data (filter, NULL));
    if (cfilter == NULL)
      goto invalid_filter;
  } else {
    cfilter = NULL;
  }

  g_signal_emit (port, signals[SIGNAL_FORMAT_REQUEST], 0, NULL);

  if (priv->possible_formats)
    caps = gst_caps_from_string (g_bytes_get_data (priv->possible_formats, NULL));
  else
    caps = gst_caps_new_any ();

  if (caps && cfilter) {
    tmp = gst_caps_intersect_full (caps, cfilter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_take (&caps, tmp);
  }
  g_clear_pointer (&cfilter, gst_caps_unref);

  if (caps == NULL || gst_caps_is_empty (caps))
    goto no_format;

  str = gst_caps_to_string (caps);
  gst_caps_unref (caps);
  res = g_bytes_new_take (str, strlen (str) + 1);

  if (priv->direction == PINOS_DIRECTION_OUTPUT) {
    guint i;
    for (i = 0; i < priv->peers->len; i++) {
      PinosPort *peer = g_ptr_array_index (priv->peers, i);
      res = pinos_port_filter_formats (peer, res, error);
    }
  }

  return res;

invalid_filter:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_INVALID_ARGUMENT,
                 "Invalid filter received");
    return NULL;
  }
no_format:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_FOUND,
                 "No compatible format found");
    if (cfilter)
      gst_caps_unref (cfilter);
    if (caps)
      gst_caps_unref (caps);
    return NULL;
  }
}

static void
parse_control_buffer (PinosPort *port, PinosBuffer *buffer)
{
  PinosPortPrivate *priv = port->priv;
  PinosBufferIter it;

  pinos_buffer_iter_init (&it, buffer);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange change;

        if (!pinos_buffer_iter_parse_format_change  (&it, &change))
          continue;

        if (priv->format)
          g_bytes_unref (priv->format);
        priv->format = g_bytes_new (change.format, strlen (change.format) + 1);
        g_object_notify (G_OBJECT (port), "format");
        break;
      }
      default:
        break;
    }
  }
}

static PinosBuffer *
read_buffer (PinosPort    *port,
             GError      **error)
{
  PinosPortPrivate *priv = port->priv;
  gssize len;
  PinosStackHeader *hdr;
  PinosStackBuffer *sb = (PinosStackBuffer *) &priv->recv_buffer;
  gsize need;
  struct cmsghdr *cmsg;
  struct msghdr msg = {0};
  struct iovec iov[1];
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];

  g_assert (sb->refcount == 0);

  sb->data = priv->recv_data;
  sb->max_size = MAX_BUFFER_SIZE;
  sb->size = 0;
  sb->free_data = NULL;
  sb->fds = priv->recv_fds;
  sb->max_fds = 0;
  sb->n_fds = 0;
  sb->free_fds = NULL;

  hdr = sb->data;

  /* read header and control messages first */
  iov[0].iov_base = hdr;
  iov[0].iov_len = sizeof (PinosStackHeader);;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);
  msg.msg_flags = MSG_CMSG_CLOEXEC;

  while (TRUE) {
    len = recvmsg (priv->fd, &msg, msg.msg_flags);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto recv_error;
    }
    break;
  }
  g_assert (len == sizeof (PinosStackHeader));

  /* now we know the total length */
  need = sizeof (PinosStackHeader) + hdr->length;

  if (sb->max_size < need) {
    g_warning ("port %p: realloc receive memory %" G_GSIZE_FORMAT" -> %" G_GSIZE_FORMAT, port, sb->max_size, need);
    sb->max_size = need;
    hdr = sb->data = sb->free_data = g_realloc (sb->free_data, need);
  }
  sb->size = need;

  if (hdr->length > 0) {
    /* read data */
    while (TRUE) {
      len = recv (priv->fd, (gchar *)sb->data + sizeof (PinosStackHeader), hdr->length, 0);
      if (len < 0) {
        if (errno == EINTR)
          continue;
        else
          goto recv_error;
      }
      break;
    }
    g_assert (len == hdr->length);
  }

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    sb->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (sb->fds, CMSG_DATA (cmsg), sb->n_fds * sizeof (int));
  }
  sb->refcount = 1;
  sb->magic = PSB_MAGIC;

  PINOS_DEBUG_TRANSPORT ("port %p: buffer %p init", &priv->recv_buffer, sb);

  return &priv->recv_buffer;

  /* ERRORS */
recv_error:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not recvmsg: %s", strerror (errno));
    return NULL;
  }
}


static gboolean
write_buffer (PinosPort   *port,
              PinosBuffer *buffer,
              GError     **error)
{
  PinosPortPrivate *priv = port->priv;
  PinosStackBuffer *sb = (PinosStackBuffer *) buffer;
  gssize len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];
  gint fds_len = sb->n_fds * sizeof (int);

  iov[0].iov_base = sb->data;
  iov[0].iov_len = sb->size;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = CMSG_SPACE (fds_len);
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (fds_len);
  memcpy(CMSG_DATA(cmsg), sb->fds, fds_len);
  msg.msg_controllen = cmsg->cmsg_len;

  while (TRUE) {
    len = sendmsg (priv->fd, &msg, 0);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto send_error;
    }
    break;
  }
  g_assert (len == (gssize) sb->size);

  return TRUE;

  /* ERRORS */
send_error:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not sendmsg: %s", strerror (errno));
    return FALSE;
  }
}


static gboolean
pinos_port_receive_buffer (PinosPort   *port,
                           PinosBuffer *buffer,
                           GError     **error)
{
  PinosPortPrivate *priv = port->priv;
  gboolean res;

  if (priv->buffer)
    goto buffer_queued;

  PINOS_DEBUG_TRANSPORT ("port %p: receive buffer %p", port, buffer);
  if (pinos_buffer_get_flags (buffer) & PINOS_BUFFER_FLAG_CONTROL)
    parse_control_buffer (port, buffer);

  if (priv->sockets[0]) {
    PINOS_DEBUG_TRANSPORT ("port %p: write buffer %p", port, buffer);
    res = write_buffer (port, buffer, error);
  }
  else {
    res = TRUE;
    priv->buffer = buffer;
    if (priv->received_buffer_cb)
      priv->received_buffer_cb (port, priv->received_buffer_data);
    priv->buffer = NULL;
  }

  return res;

  /* ERRORS */
buffer_queued:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_FOUND,
                 "buffer was already queued on port");
    return FALSE;
  }
}

static void
update_peer_paths (PinosPort *port)
{
  PinosPortPrivate *priv = port->priv;
  gchar **paths;
  guint i;
  gint path_index = 0;

  paths = g_new0 (gchar *, priv->peers->len + 1);
  for (i = 0; i < priv->peers->len; i++) {
    PinosPort *peer;
    gchar *path;

    peer = g_ptr_array_index (priv->peers, i);
    g_object_get (peer, "object-path", &path, NULL);
    paths[path_index++] = path;
  }

  g_object_set (port, "peers", paths, NULL);
  g_strfreev (paths);
}

/**
 * pinos_port_link:
 * @source: a source #PinosPort
 * @destination: a destination #PinosPort
 *
 * Link two ports together.
 *
 * Returns: %TRUE if ports could be linked.
 */
gboolean
pinos_port_link (PinosPort *source, PinosPort *destination)
{
  gboolean res = TRUE;

  g_return_val_if_fail (PINOS_IS_PORT (source), FALSE);
  g_return_val_if_fail (PINOS_IS_PORT (destination), FALSE);
  g_return_val_if_fail (source->priv->direction != destination->priv->direction, FALSE);

  if (source->priv->peers->len >= source->priv->max_peers)
    return FALSE;
  if (destination->priv->peers->len >= destination->priv->max_peers)
    return FALSE;

  if (source->priv->direction != PINOS_DIRECTION_OUTPUT) {
    PinosPort *tmp;
    tmp = source;
    source = destination;
    destination = tmp;
  }

  g_signal_emit (source, signals[SIGNAL_LINKED], 0, destination, &res);
  if (!res)
    return FALSE;
  g_signal_emit (destination, signals[SIGNAL_LINKED], 0, source, &res);
  if (!res)
    return FALSE;

  g_debug ("port %p: linked to %p", source, destination);
  g_ptr_array_add (source->priv->peers, destination);
  g_ptr_array_add (destination->priv->peers, source);

  update_peer_paths (source);
  update_peer_paths (destination);

  if (source->priv->format) {
    PinosBufferBuilder builder;
    PinosBuffer pbuf;
    PinosPacketFormatChange fc;
    GError *error = NULL;

    pinos_port_buffer_builder_init (destination, &builder);
    fc.id = 0;
    fc.format = g_bytes_get_data (source->priv->format, NULL);
    pinos_buffer_builder_add_format_change (&builder, &fc);
    pinos_buffer_builder_end (&builder, &pbuf);

    if (!pinos_port_receive_buffer (destination, &pbuf, &error)) {
      g_warning ("port %p: counld not receive format: %s", destination, error->message);
      g_clear_error (&error);
    }
    pinos_buffer_unref (&pbuf);
  }

  return TRUE;
}

/**
 * pinos_port_unlink:
 * @source: a source #PinosPort
 * @destination: a destination #PinosPort
 *
 * Link two ports together.
 *
 * Returns: %TRUE if ports could be linked.
 */
gboolean
pinos_port_unlink (PinosPort *source, PinosPort *destination)
{
  g_return_val_if_fail (PINOS_IS_PORT (source), FALSE);
  g_return_val_if_fail (PINOS_IS_PORT (destination), FALSE);

  g_ptr_array_remove (source->priv->peers, destination);
  g_ptr_array_remove (destination->priv->peers, source);

  update_peer_paths (source);
  update_peer_paths (destination);

  g_debug ("port %p: unlinked from %p", source, destination);
  g_signal_emit (source, signals[SIGNAL_UNLINKED], 0, destination);
  g_signal_emit (destination, signals[SIGNAL_UNLINKED], 0, source);

  return TRUE;
}

static void
pinos_port_unlink_all (PinosPort *port)
{
  PinosPortPrivate *priv = port->priv;
  guint i;

  for (i = 0; i < priv->peers->len; i++) {
    PinosPort *peer = g_ptr_array_index (priv->peers, i);

    g_ptr_array_remove (peer->priv->peers, port);
    g_ptr_array_index (priv->peers, i) = NULL;

    g_signal_emit (port, signals[SIGNAL_UNLINKED], 0, peer);
    g_signal_emit (peer, signals[SIGNAL_UNLINKED], 0, port);
  }
  g_ptr_array_set_size (priv->peers, 0);
}

/**
 * pinos_port_get_links:
 * @port: a #PinosPort
 * @n_linkes: location to hold the result number of links
 *
 * Get the links and number of links on this port
 *
 * Returns: an array of @n_links elements of type #PinosPort.
 */
PinosPort *
pinos_port_get_links (PinosPort *port, guint *n_links)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  if (n_links)
    *n_links = priv->peers->len;

  return (PinosPort *) priv->peers->pdata;
}

static gboolean
on_socket_condition (GSocket      *socket,
                     GIOCondition  condition,
                     gpointer      user_data)
{
  PinosPort *port = user_data;
  PinosPortPrivate *priv = port->priv;
  GError *error = NULL;

  switch (condition) {
    case G_IO_IN:
    {
      guint i;
      PinosBuffer *buffer;

      buffer = read_buffer (port, &error);
      if (buffer == NULL) {
        g_warning ("port %p: failed to read buffer: %s", port, error->message);
        g_clear_error (&error);
        return TRUE;
      }

      if (pinos_buffer_get_flags (buffer) & PINOS_BUFFER_FLAG_CONTROL)
        parse_control_buffer (port, buffer);

      PINOS_DEBUG_TRANSPORT ("port %p: read buffer %p", port, buffer);

      if (priv->received_buffer_cb) {
        PINOS_DEBUG_TRANSPORT ("port %p: notify buffer %p", port, buffer);
        priv->buffer = buffer;
        priv->received_buffer_cb (port, priv->received_buffer_data);
        priv->buffer = NULL;
      }
      PINOS_DEBUG_TRANSPORT ("port %p: send to peer buffer %p", port, buffer);
      for (i = 0; i < priv->peers->len; i++) {
        PinosPort *peer = g_ptr_array_index (priv->peers, i);

        if (!pinos_port_receive_buffer (peer, buffer, &error)) {
          g_warning ("peer %p: failed to receive buffer: %s", peer, error->message);
          g_clear_error (&error);
        }
      }
      g_assert (pinos_buffer_unref (buffer) == FALSE);
      break;
    }

    case G_IO_OUT:
      g_warning ("can do IO OUT\n");
      break;

    default:
      break;
  }
  return TRUE;
}


static void
handle_socket (PinosPort *port, GSocket *socket)
{
  PinosPortPrivate *priv = port->priv;
  GMainContext *context = g_main_context_get_thread_default();

  g_debug ("port %p: handle socket in context %p", port, context);
  priv->fd = g_socket_get_fd (socket);
  priv->socket_source = g_socket_create_source (socket, G_IO_IN, NULL);
  g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, port, NULL);
  g_source_attach (priv->socket_source, context);
}

static void
unhandle_socket (PinosPort *port)
{
  PinosPortPrivate *priv = port->priv;

  g_debug ("port %p: unhandle socket", port);
  if (priv->socket_source) {
    g_source_destroy (priv->socket_source);
    g_clear_pointer (&priv->socket_source, g_source_unref);
    priv->fd = -1;
  }
}

/**
 * pinos_port_peek_buffer:
 * @port: a #PinosPort
 *
 * Peek the buffer on @port.
 *
 * Returns: a #PinosBuffer or %NULL when no buffer has arrived on the pad.
 */
PinosBuffer *
pinos_port_peek_buffer (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->buffer;
}

void
pinos_port_buffer_builder_init (PinosPort   *port,
                                PinosBufferBuilder *builder)
{
  PinosPortPrivate *priv;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;

  pinos_buffer_builder_init_into (builder,
                                  priv->send_data, MAX_BUFFER_SIZE,
                                  priv->send_fds, MAX_FDS);
}

/**
 * pinos_port_send_buffer:
 * @port: a #PinosPort
 * @buffer: a #PinosBuffer
 * @error: a #GError or %NULL
 *
 * Send @buffer to ports connected to @port
 *
 * Returns: %TRUE on success. @error is set when %FALSE is returned.
 */
gboolean
pinos_port_send_buffer (PinosPort   *port,
                        PinosBuffer *buffer,
                        GError     **error)
{
  PinosPortPrivate *priv;
  PinosPort *peer;
  gboolean res = TRUE;
  guint i;
  GError *err = NULL;

  g_return_val_if_fail (PINOS_IS_PORT (port), FALSE);
  priv = port->priv;

  if (pinos_buffer_get_flags (buffer) & PINOS_BUFFER_FLAG_CONTROL)
    parse_control_buffer (port, buffer);

  PINOS_DEBUG_TRANSPORT ("port %p: send buffer %p", port, buffer);
  if (priv->sockets[0]) {
    PINOS_DEBUG_TRANSPORT ("port %p: write buffer %p", port, buffer);
    res = write_buffer (port, buffer, &err);
  }
  for (i = 0; i < priv->peers->len; i++) {
    peer = g_ptr_array_index (priv->peers, i);
    res = pinos_port_receive_buffer (peer, buffer, &err);
  }
  if (!res) {
    if (error == NULL)
      g_warning ("could not send buffer: %s", err->message);
    g_propagate_error (error, err);
  }

  return res;
}

/**
 * pinos_port_get_socket_pair:
 * @port: a #PinosPort
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @port. The
 * Socket for the other end is returned.
 *
 * Returns: a #GSocket that can be used to send buffers to port.
 */
GSocket *
pinos_port_get_socket_pair (PinosPort *port,
                            GError   **error)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), FALSE);
  priv = port->priv;

  if (priv->sockets[1] == NULL) {
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      goto no_sockets;

    priv->sockets[0] = g_socket_new_from_fd (fd[0], error);
    if (priv->sockets[0] == NULL)
      goto create_failed;

    priv->sockets[1] = g_socket_new_from_fd (fd[1], error);
    if (priv->sockets[1] == NULL)
      goto create_failed;

    handle_socket (port, priv->sockets[0]);
  }
  return g_object_ref (priv->sockets[1]);

  /* ERRORS */
no_sockets:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not create socketpair: %s", strerror (errno));
    return NULL;
  }
create_failed:
  {
    g_clear_object (&priv->sockets[0]);
    g_clear_object (&priv->sockets[1]);
    return NULL;
  }
}

static void
pinos_port_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PinosPort *port = PINOS_PORT (_object);
  PinosPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_NODE:
      g_value_set_object (value, priv->node);
      break;

    case PROP_SOCKET:
      g_value_set_object (value, priv->sockets[0]);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_MAX_PEERS:
      g_value_set_uint (value, priv->max_peers);
      break;

    case PROP_PEERS:
      g_value_set_boxed (value, priv->peer_paths);
      break;

    case PROP_DIRECTION:
      g_value_set_enum (value, priv->direction);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_FORMAT:
      g_value_set_boxed (value, priv->format);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_port_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosPort *port = PINOS_PORT (_object);
  PinosPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_NODE:
      priv->node = g_value_get_object (value);
      break;

    case PROP_SOCKET:
      priv->sockets[0] = g_value_dup_object (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_DIRECTION:
      priv->direction = g_value_get_enum (value);
      break;

    case PROP_MAX_PEERS:
      priv->max_peers = g_value_get_uint (value);
      break;

    case PROP_PEERS:
      if (priv->peer_paths)
        g_strfreev (priv->peer_paths);
      priv->peer_paths = g_value_dup_boxed (value);
      break;

    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_bytes_unref (priv->format);
      priv->format = g_value_dup_boxed (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_port_constructed (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);
  PinosPortPrivate *priv = port->priv;

  g_debug ("port %p: %s port constructed, node %p",
      port, pinos_direction_as_string (priv->direction), priv->node);

  if (priv->sockets[0])
    handle_socket (port, priv->sockets[0]);

  if (priv->direction == PINOS_DIRECTION_OUTPUT)
    priv->max_peers = G_MAXUINT;
  else
    priv->max_peers = 1;

  G_OBJECT_CLASS (pinos_port_parent_class)->constructed (object);
}

static void
pinos_port_dispose (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);
  PinosPortPrivate *priv = port->priv;

  g_debug ("port %p: dispose", port);
  if (priv->sockets[0]) {
    unhandle_socket (port);
    g_clear_object (&priv->sockets[0]);
  }
  g_clear_object (&priv->sockets[1]);
  pinos_port_unlink_all (port);

  G_OBJECT_CLASS (pinos_port_parent_class)->dispose (object);
}

static void
pinos_port_finalize (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);
  PinosPortPrivate *priv = port->priv;

  g_debug ("port %p: finalize", port);

  g_free (priv->name);
  g_clear_pointer (&priv->possible_formats, g_bytes_unref);
  g_clear_pointer (&priv->format, g_bytes_unref);
  g_clear_pointer (&priv->properties, pinos_properties_free);
  if (priv->received_buffer_notify)
    priv->received_buffer_notify (priv->received_buffer_data);
  g_ptr_array_unref (priv->peers);

  G_OBJECT_CLASS (pinos_port_parent_class)->finalize (object);
}

static gboolean
signal_linked_handler (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  return TRUE;
}

static gboolean
signal_linked_accum (GSignalInvocationHint *ihint,
                     GValue *return_accu,
                     const GValue *handler_return,
                     gpointer data)
{
  if (!g_value_get_boolean (handler_return)) {
    g_value_set_boolean (return_accu, FALSE);
    return FALSE;
  }
  g_value_set_boolean (return_accu, TRUE);
  return TRUE;
}


static void
pinos_port_class_init (PinosPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosPortPrivate));

  gobject_class->constructed = pinos_port_constructed;
  gobject_class->dispose = pinos_port_dispose;
  gobject_class->finalize = pinos_port_finalize;
  gobject_class->set_property = pinos_port_set_property;
  gobject_class->get_property = pinos_port_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_NODE,
                                   g_param_spec_object ("node",
                                                        "Node",
                                                        "The Node",
                                                        PINOS_TYPE_NODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SOCKET,
                                   g_param_spec_object ("socket",
                                                        "Socket",
                                                        "The socket for this port",
                                                        G_TYPE_SOCKET,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The port name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_DIRECTION,
                                   g_param_spec_enum ("direction",
                                                      "Direction",
                                                      "The direction of the port",
                                                      PINOS_TYPE_DIRECTION,
                                                      PINOS_DIRECTION_INVALID,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_MAX_PEERS,
                                   g_param_spec_uint ("max-peers",
                                                      "Max Peers",
                                                      "The maximum number of peer ports",
                                                      1, G_MAXUINT, G_MAXUINT,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PEERS,
                                   g_param_spec_boxed ("peers",
                                                       "Peers",
                                                       "The peer ports of the port",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possbile formats of the port",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FORMAT,
                                   g_param_spec_boxed ("format",
                                                       "Format",
                                                       "The format of the port",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the port",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));


  signals[SIGNAL_FORMAT_REQUEST] = g_signal_new ("format-request",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_generic,
                                                 G_TYPE_NONE,
                                                 0,
                                                 G_TYPE_NONE);

  signals[SIGNAL_REMOVE] = g_signal_new ("remove",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         g_cclosure_marshal_generic,
                                         G_TYPE_NONE,
                                         0,
                                         G_TYPE_NONE);
  signals[SIGNAL_LINKED] = g_signal_new_class_handler ("linked",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         (GCallback) signal_linked_handler,
                                         signal_linked_accum,
                                         NULL,
                                         g_cclosure_marshal_generic,
                                         G_TYPE_BOOLEAN,
                                         1,
                                         PINOS_TYPE_PORT);
  signals[SIGNAL_UNLINKED] = g_signal_new ("unlinked",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL,
                                           NULL,
                                           g_cclosure_marshal_generic,
                                           G_TYPE_NONE,
                                           1,
                                           PINOS_TYPE_PORT);
}

static void
pinos_port_init (PinosPort * port)
{
  PinosPortPrivate *priv = port->priv = PINOS_PORT_GET_PRIVATE (port);

  priv->direction = PINOS_DIRECTION_INVALID;
  priv->peers = g_ptr_array_new_full (64, NULL);
}