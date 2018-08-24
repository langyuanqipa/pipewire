/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <regex.h>

#include <jack/jack.h>
#include <jack/session.h>
#include <jack/thread.h>

#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include "extensions/client-node.h"

#define JACK_CLIENT_NAME_SIZE		64
#define JACK_PORT_NAME_SIZE		256
#define JACK_PORT_MAX			4096
#define JACK_PORT_TYPE_SIZE             32
#define CONNECTION_NUM_FOR_PORT		2048

#define BUFFER_SIZE_MAX			8192

#define MAX_OBJECTS			8192
#define MAX_PORTS			1024
#define MAX_BUFFERS			2
#define MAX_BUFFER_DATAS		4u
#define MAX_BUFFER_MEMS			4
#define MAX_MIX				4096
#define MAX_IO				32

#define DEFAULT_SAMPLE_RATE	48000
#define DEFAULT_BUFFER_SIZE	1024
#define MAX_BUFFER_SIZE		2048

#define REAL_JACK_PORT_NAME_SIZE (JACK_CLIENT_NAME_SIZE + JACK_PORT_NAME_SIZE)

#define NAME	"jack-client"

struct client;
struct port;

struct globals {
	jack_thread_creator_t creator;
};

static struct globals globals;

#define OBJECT_CHUNK	8

struct object {
	struct spa_list link;

	struct client *client;

	uint32_t type;
	uint32_t id;
	uint32_t parent_id;

	union {
		struct {
			char name[JACK_CLIENT_NAME_SIZE+1];
		} node;
		struct {
			uint32_t src;
			uint32_t dst;
		} port_link;
		struct {
			unsigned long flags;
			char name[REAL_JACK_PORT_NAME_SIZE+1];
			char alias1[REAL_JACK_PORT_NAME_SIZE+1];
			char alias2[REAL_JACK_PORT_NAME_SIZE+1];
			uint32_t type_id;
			uint32_t port_id;
		} port;
	};
};

struct mem {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct pw_map_range map;
	void *ptr;
};

struct midi_buffer {
#define MIDI_BUFFER_MAGIC 0x900df00d
	uint32_t magic;
	int32_t buffer_size;
	uint32_t nframes;
	int32_t write_pos;
	uint32_t event_count;
	uint32_t lost_events;
};

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1<<0)
#define BUFFER_FLAG_MAPPED	(1<<1)
	uint32_t flags;
	uint32_t id;
	void *ptr;
	struct pw_map_range map;

	struct spa_data datas[MAX_BUFFER_DATAS];
	uint32_t n_datas;

	uint32_t mem[MAX_BUFFER_DATAS+1];
	uint32_t n_mem;
};

struct io {
	uint32_t id;
	uint32_t memid;
};

struct mix {
	struct spa_list link;
	struct spa_list port_link;
	uint32_t id;
	struct port *port;

	struct io ios[MAX_IO];

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	struct spa_list queue;
};

struct port {
	bool valid;
	struct spa_list link;

	struct client *client;

	enum spa_direction direction;
	uint32_t id;
	struct object *object;

	struct spa_list mix;

	bool have_format;
	uint32_t rate;

	bool zeroed;
	float empty[BUFFER_SIZE_MAX + 8];
};

struct context {
	struct pw_main_loop *main;
	struct pw_thread_loop *loop;
	struct pw_core *core;

	struct pw_map globals;
	struct spa_list free_objects;
	struct spa_list ports;
	struct spa_list nodes;
	struct spa_list links;
};

#define GET_DIRECTION(f)	((f) & JackPortIsInput ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT)

#define GET_IN_PORT(c,p)	(&c->port_pool[SPA_DIRECTION_INPUT][p])
#define GET_OUT_PORT(c,p)	(&c->port_pool[SPA_DIRECTION_OUTPUT][p])
#define GET_PORT(c,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(c,p) : GET_OUT_PORT(c,p))

struct client {
	char name[JACK_CLIENT_NAME_SIZE+1];

	struct context context;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	uint32_t last_sync;
	bool error;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_listener;
        struct spa_hook proxy_listener;

	uint32_t node_id;
	int writefd;
	struct spa_source *socket_source;

	bool active;

	JackThreadCallback thread_callback;
	void *thread_arg;
	JackThreadInitCallback thread_init_callback;
	void *thread_init_arg;
	JackShutdownCallback shutdown_callback;
	void *shutdown_arg;
	JackInfoShutdownCallback info_shutdown_callback;
	void *info_shutdown_arg;
	JackProcessCallback process_callback;
	void *process_arg;
	JackFreewheelCallback freewheel_callback;
	void *freewheel_arg;
	JackBufferSizeCallback bufsize_callback;
	void *bufsize_arg;
	JackSampleRateCallback srate_callback;
	void *srate_arg;
	JackClientRegistrationCallback registration_callback;
	void *registration_arg;
	JackPortRegistrationCallback portregistration_callback;
	void *portregistration_arg;
	JackPortConnectCallback connect_callback;
	void *connect_arg;
	JackGraphOrderCallback graph_callback;
	void *graph_arg;
	JackSyncCallback sync_callback;
	void *sync_arg;
	JackTimebaseCallback timebase_callback;
	void *timebase_arg;

	struct pw_client_node_position *position;
	struct pw_driver_quantum *quantum;
	uint32_t sample_rate;
	uint32_t buffer_size;

	struct spa_list mix[2];
	struct mix mix_pool[MAX_MIX];
	struct spa_list free_mix;

	struct port port_pool[2][MAX_PORTS];
	struct spa_list ports[2];
	struct spa_list free_ports[2];

        struct pw_array mems;

	bool started;
	int status;

	jack_position_t jack_position;
};

static void init_port_pool(struct client *c, enum spa_direction direction)
{
	int i;

	spa_list_init(&c->ports[direction]);
	spa_list_init(&c->free_ports[direction]);
	for (i = 0; i < MAX_PORTS; i++) {
		c->port_pool[direction][i].direction = direction;
		c->port_pool[direction][i].id = i;
		spa_list_append(&c->free_ports[direction], &c->port_pool[direction][i].link);
	}
}

static struct object * alloc_object(struct client *c)
{
	struct object *o;
	int i;

	if (spa_list_is_empty(&c->context.free_objects)) {
		o = calloc(OBJECT_CHUNK, sizeof(struct object));
		if (o == NULL)
			return NULL;
		for (i = 0; i < OBJECT_CHUNK; i++)
			spa_list_append(&c->context.free_objects, &o[i].link);
	}

        o = spa_list_first(&c->context.free_objects, struct object, link);
        spa_list_remove(&o->link);
	o->client = c;

	return o;
}

static void free_object(struct client *c, struct object *o)
{
        spa_list_remove(&o->link);
	spa_list_append(&c->context.free_objects, &o->link);
}

static struct mix *ensure_mix(struct client *c, struct port *port, uint32_t mix_id)
{
	struct mix *mix;
	int i;

	spa_list_for_each(mix, &port->mix, port_link) {
		if (mix->id == mix_id)
			return mix;
	}
	if (spa_list_is_empty(&c->free_mix))
		return NULL;

	mix = spa_list_first(&c->free_mix, struct mix, link);
	spa_list_remove(&mix->link);

	spa_list_append(&c->mix[port->direction], &mix->link);
	spa_list_append(&port->mix, &mix->port_link);

	mix->id = mix_id;
	mix->port = port;
	mix->io = NULL;
	mix->n_buffers = 0;
	for (i = 0; i < MAX_IO; i++)
		mix->ios[i].id = SPA_ID_INVALID;

	return mix;
}

static void free_mix(struct client *c, struct mix *mix)
{
	spa_list_remove(&mix->link);
	spa_list_remove(&mix->port_link);
	spa_list_append(&c->free_mix, &mix->link);
}

static struct port * alloc_port(struct client *c, enum spa_direction direction)
{
	struct port *p;
	struct object *o;

	if (spa_list_is_empty(&c->free_ports[direction]))
		return NULL;

	p = spa_list_first(&c->free_ports[direction], struct port, link);
	spa_list_remove(&p->link);

	o = alloc_object(c);
	o->type = PW_ID_INTERFACE_Port;
	o->id = SPA_ID_INVALID;
	o->parent_id = c->node_id;
	o->port.port_id = p->id;
	spa_list_append(&c->context.ports, &o->link);

	p->valid = true;
	p->zeroed = false;
	p->client = c;
	p->object = o;
	spa_list_init(&p->mix);

	spa_list_append(&c->ports[direction], &p->link);

	return p;
}

static void free_port(struct client *c, struct port *p)
{
	struct mix *m, *t;

	if (!p->valid)
		return;

	spa_list_for_each_safe(m, t, &p->mix, port_link)
		free_mix(c, m);

	spa_list_remove(&p->link);
	p->valid = false;
	free_object(c, p->object);
	spa_list_append(&c->free_ports[p->direction], &p->link);
}

static struct object *find_port(struct client *c, const char *name)
{
	struct object *o;

	spa_list_for_each(o, &c->context.ports, link) {
		if (!strcmp(o->port.name, name))
			return o;
	}
	return NULL;
}

static struct object *find_link(struct client *c, uint32_t src, uint32_t dst)
{
	struct object *l;

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == src &&
		    l->port_link.dst == dst) {
			return l;
		}
	}
	return NULL;
}

static struct buffer *dequeue_buffer(struct mix *mix)
{
        struct buffer *b;

        if (spa_list_is_empty(&mix->queue))
                return NULL;

        b = spa_list_first(&mix->queue, struct buffer, link);
        spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

        return b;
}

void jack_get_version(int *major_ptr, int *minor_ptr, int *micro_ptr, int *proto_ptr)
{
	*major_ptr = 0;
	*minor_ptr = 0;
	*micro_ptr = 0;
	*proto_ptr = 0;
}

const char *
jack_get_version_string(void)
{
	return "0.0.0.0";
}

static void on_sync_reply(void *data, uint32_t seq)
{
	struct client *client = data;
	client->last_sync = seq;
	pw_thread_loop_signal(client->context.loop, false);
}

static void on_state_changed(void *data, enum pw_remote_state old,
                             enum pw_remote_state state, const char *error)
{
	struct client *client = data;

	pw_log_debug(NAME" %p: state %s", client, pw_remote_state_as_string(state));
	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		client->error = true;
		/* fallthrough*/
        case PW_REMOTE_STATE_UNCONNECTED:
		if (client->shutdown_callback)
			client->shutdown_callback(client->shutdown_arg);
		/* fallthrough*/
        case PW_REMOTE_STATE_CONNECTED:
		pw_thread_loop_signal(client->context.loop, false);
                break;
        default:
                break;
        }
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.sync_reply = on_sync_reply,
	.state_changed = on_state_changed,
};

static int do_sync(struct client *client)
{
	uint32_t seq = client->last_sync + 1;

	pw_core_proxy_sync(client->core_proxy, seq);

	while (true) {
	        pw_thread_loop_wait(client->context.loop);

		if (client->error)
			return -1;

		if (client->last_sync == seq)
			break;
	}
	return 0;
}

static void on_node_proxy_destroy(void *data)
{
	struct client *client = data;

	client->node_proxy = NULL;
	spa_hook_remove(&client->proxy_listener);

}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_node_proxy_destroy,
};

static struct mem *find_mem(struct pw_array *mems, uint32_t id)
{
	struct mem *m;

	pw_array_for_each(m, mems) {
		if (m->id == id)
			return m;
	}
	return NULL;
}

static struct mem *find_mem_ptr(struct pw_array *mems, void *ptr)
{
	struct mem *m;
	pw_array_for_each(m, mems) {
		if (m->ptr == ptr)
			return m;
	}
	return NULL;
}


static void *mem_map(struct client *c, struct mem *m, uint32_t offset, uint32_t size)
{
	struct pw_map_range map;

	pw_map_range_init(&map, offset, size, c->context.core->sc_pagesize);

	if (m->ptr == NULL || m->map.offset != map.offset || m->map.size != map.size) {
		m->ptr = mmap(m->ptr, map.size, PROT_READ|PROT_WRITE,
				MAP_SHARED, m->fd, map.offset);

		if (m->ptr == MAP_FAILED) {
			pw_log_error(NAME" %p: Failed to mmap memory %d %p: %m", c, size, m);
			m->ptr = NULL;
			return NULL;
		}
		m->map = map;
	}
	return SPA_MEMBER(m->ptr, map.start, void);
}

static void mem_unmap(struct client *c, struct mem *m)
{
	pw_log_debug(NAME" %p: %d fd %d", c, m->id, m->fd);
	if (m->ptr != NULL) {
		if (munmap(m->ptr, m->map.size) < 0)
			pw_log_warn(NAME" %p: failed to unmap: %m", c);
		m->ptr = NULL;
	}
}

static void clear_mem(struct client *c, struct mem *m)
{
	pw_log_debug(NAME" %p: %d fd %d", c, m->id, m->fd);
	if (m->fd != -1) {
		bool has_ref = false;
		struct mem *m2;

		pw_array_for_each(m2, &c->mems) {
			if (m2 != m && m2->fd == m->fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			mem_unmap(c, m);
			pw_log_debug(NAME" %p: close %d fd %d", c, m->id, m->fd);
			close(m->fd);
			m->id = -1;
			m->fd = -1;
		}
	}
}

static void client_node_add_mem(void *object,
				uint32_t mem_id,
				uint32_t type,
				int memfd,
				uint32_t flags)
{
	struct client *c = object;
	struct mem *m;

	m = find_mem(&c->mems, mem_id);
	if (m) {
		pw_log_warn(NAME" %p: duplicate mem %u, fd %d, flags %d", c,
			     mem_id, memfd, flags);
		return;
	}

	m = pw_array_add(&c->mems, sizeof(struct mem));
	pw_log_debug(NAME" %p: add mem %u, fd %d, flags %d", c, mem_id, memfd, flags);

	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->ref = 0;
	m->map = PW_MAP_RANGE_INIT;
	m->ptr = NULL;
}

static int
do_remove_sources(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;

	if (c->socket_source) {
		pw_loop_destroy_source(c->context.core->data_loop, c->socket_source);
		c->socket_source = NULL;
	}
	if (c->writefd != -1) {
		close(c->writefd);
		c->writefd = -1;
	}
	return 0;
}

static void unhandle_socket(struct client *c)
{
        pw_loop_invoke(c->context.core->data_loop,
                       do_remove_sources, 1, NULL, 0, true, c);
}

static void reuse_buffer(struct client *c, struct mix *mix, uint32_t id)
{
	struct buffer *b;

	b = &mix->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		pw_log_trace(NAME" %p: port %p: recycle buffer %d", c, mix->port, id);
		spa_list_append(&mix->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
	}
}

static void
on_rtsocket_condition(void *data, int fd, enum spa_io mask)
{
	struct client *c = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(c);
		return;
	}

	if (mask & SPA_IO_IN) {
		uint64_t cmd;
		uint32_t buffer_size, sample_rate;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			pw_log_warn("jack %p: read failed %m", c);

		buffer_size = c->quantum->size;
		if (buffer_size != c->buffer_size) {
			pw_log_info("jack %p: buffersize %d", c, buffer_size);
			c->buffer_size = buffer_size;
			if (c->bufsize_callback)
				c->bufsize_callback(c->buffer_size, c->bufsize_arg);
		}

		if (c->quantum->rate.num != 0 && c->quantum->rate.denom != 0)
			sample_rate = c->quantum->rate.denom / c->quantum->rate.num;
		else
			sample_rate = c->sample_rate;

		if (sample_rate != c->sample_rate) {
			pw_log_info("jack %p: sample_rate %d", c, sample_rate);
			c->sample_rate = sample_rate;
			if (c->srate_callback)
				c->srate_callback(c->sample_rate, c->srate_arg);
		}

		c->jack_position.unique_1++;
		c->jack_position.usecs = c->quantum->nsec/1000;
		c->jack_position.frame_rate = c->sample_rate;
		c->jack_position.frame = c->quantum->position;
		c->jack_position.valid = 0;
		c->jack_position.unique_2 = c->jack_position.unique_1;

		if (c->sync_callback) {
			c->sync_callback(JackTransportRolling,
					 &c->jack_position, c->sync_arg);
		}

		pw_log_trace("do process %d %d %d %"PRIi64, c->buffer_size, c->sample_rate,
				c->jack_position.frame, c->quantum->delay);

		if (c->process_callback)
			c->process_callback(c->buffer_size, c->process_arg);

		if (c->timebase_callback) {
			c->timebase_callback(JackTransportRolling,
					     buffer_size,
					     &c->jack_position,
					     false,
					     c->timebase_arg);
		}

		cmd = 1;
		write(c->writefd, &cmd, 8);
	}
}

static void clean_transport(struct client *c)
{
	struct mem *m;

	if (c->node_id == SPA_ID_INVALID)
		return;

	unhandle_socket(c);

	pw_array_for_each(m, &c->mems)
		clear_mem(c, m);
	pw_array_clear(&c->mems);

	close(c->writefd);

	c->node_id = SPA_ID_INVALID;
}

static void client_node_transport(void *object,
                           uint32_t node_id,
                           int readfd,
                           int writefd)
{
	struct client *c = (struct client *) object;
	struct pw_core *core = c->context.core;

	clean_transport(c);

	c->node_id = node_id;

	pw_log_debug("client %p: create client transport with fds %d %d for node %u",
			c, readfd, writefd, node_id);

	c->writefd = writefd;
        c->socket_source = pw_loop_add_io(core->data_loop,
					  readfd,
					  SPA_IO_ERR | SPA_IO_HUP,
					  true, on_rtsocket_condition, c);
}

static void client_node_set_param(void *object, uint32_t seq,
                           uint32_t id, uint32_t flags,
                           const struct spa_pod *param)
{
	struct client *c = (struct client *) object;
	pw_client_node_proxy_done(c->node_proxy, seq, -ENOTSUP);
}

static void client_node_set_io(void *object,
			       uint32_t id,
			       uint32_t mem_id,
			       uint32_t offset,
			       uint32_t size)
{
	struct client *c = (struct client *) object;
        struct mem *m;
        void *ptr;

        if (mem_id == SPA_ID_INVALID) {
                ptr = NULL;
                size = 0;
        }
        else {
                m = find_mem(&c->mems, mem_id);
                if (m == NULL) {
                        pw_log_warn("unknown memory id %u", mem_id);
			return;
                }
                if ((ptr = mem_map(c, m, offset, size)) == NULL) {
			return;
		}
		m->ref++;
        }
	pw_log_debug("client %p: set io %s %p", c,
			spa_debug_type_find_name(spa_debug_types, id), ptr);

	if (id == PW_ID_IO_ClientNodePosition) {
		if (ptr == NULL && c->position) {
			m = find_mem_ptr(&c->mems, c->position);
			if (m && --m->ref == 0)
				clear_mem(c, m);
		}
		c->position = ptr;
		if (ptr)
			c->quantum = SPA_MEMBER(ptr, sizeof(struct pw_client_node_position), void);
		else
			c->quantum = NULL;
	}
}

static void client_node_event(void *object, const struct spa_event *event)
{
}

static void client_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct client *c = (struct client *) object;

	pw_log_debug("got command %d", SPA_COMMAND_TYPE(command));
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Pause:
		if (c->started) {
			pw_loop_update_io(c->context.core->data_loop,
					  c->socket_source, SPA_IO_ERR | SPA_IO_HUP);

			c->started = false;
		}
		pw_client_node_proxy_done(c->node_proxy, seq, 0);
		break;

	case SPA_NODE_COMMAND_Start:
		if (!c->started) {
			pw_loop_update_io(c->context.core->data_loop,
					  c->socket_source,
					  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
			c->started = true;
		}
		pw_client_node_proxy_done(c->node_proxy, seq, 0);
		break;
	default:
		pw_log_warn("unhandled node command %d", SPA_COMMAND_TYPE(command));
		pw_client_node_proxy_done(c->node_proxy, seq, -ENOTSUP);
	}
}

static void client_node_add_port(void *object,
                          uint32_t seq,
                          enum spa_direction direction,
                          uint32_t port_id)
{
	struct client *c = (struct client *) object;
	pw_client_node_proxy_done(c->node_proxy, seq, -ENOTSUP);
}

static void client_node_remove_port(void *object,
                             uint32_t seq,
                             enum spa_direction direction,
                             uint32_t port_id)
{
	struct client *c = (struct client *) object;
	pw_client_node_proxy_done(c->node_proxy, seq, -ENOTSUP);
}

static void clear_buffers(struct client *c, struct mix *mix)
{
	struct port *port = mix->port;
        struct buffer *b;
	uint32_t i, j;

        pw_log_debug(NAME" %p: port %p clear buffers", c, port);

	for (i = 0; i < mix->n_buffers; i++) {
		b = &mix->buffers[i];

		if (b->ptr != NULL) {
			if (munmap(b->ptr, b->map.size) < 0)
				pw_log_warn("failed to unmap: %m");
		}
		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];
			if (d->fd != -1 && d->data) {
				if (munmap(SPA_MEMBER(d->data, -d->mapoffset, void),
							d->maxsize + d->mapoffset) < 0)
					pw_log_warn("failed to unmap: %m");
			}
			d->fd = -1;
		}
		for (j = 0; j < b->n_mem; j++) {
			struct mem *m;

			if ((m = find_mem(&c->mems, b->mem[i])) == NULL)
				continue;

			if (--m->ref == 0)
				clear_mem(c, m);
		}
		b->n_mem = 0;
		b->ptr = NULL;
        }
	mix->n_buffers = 0;
	spa_list_init(&mix->queue);
}

static int param_enum_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case 0:
		*param = spa_pod_builder_object(b,
			SPA_ID_PARAM_EnumFormat, SPA_ID_OBJECT_Format,
			"I", SPA_MEDIA_TYPE_audio,
			"I", SPA_MEDIA_SUBTYPE_raw,
	                ":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_F32,
	                ":", SPA_FORMAT_AUDIO_layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
	                ":", SPA_FORMAT_AUDIO_rate,     "iru", DEFAULT_SAMPLE_RATE,
				SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
	                ":", SPA_FORMAT_AUDIO_channels, "i", 1);
		break;
	case 1:
		*param = spa_pod_builder_object(b,
			SPA_ID_PARAM_EnumFormat, SPA_ID_OBJECT_Format,
			"I", SPA_MEDIA_TYPE_stream,
			"I", SPA_MEDIA_SUBTYPE_midi);
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case 0:

		*param = spa_pod_builder_object(b,
			SPA_ID_PARAM_Format, SPA_ID_OBJECT_Format,
			"I", SPA_MEDIA_TYPE_audio,
			"I", SPA_MEDIA_SUBTYPE_raw,
	                ":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_F32,
	                ":", SPA_FORMAT_AUDIO_layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
	                ":", SPA_FORMAT_AUDIO_rate,     p->have_format ? "iru" : "ir",
							p->have_format ? p->rate : DEFAULT_SAMPLE_RATE,
				SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
	                ":", SPA_FORMAT_AUDIO_channels, "i", 1);
		break;
	case 1:
		*param = spa_pod_builder_object(b,
			SPA_ID_PARAM_Format, SPA_ID_OBJECT_Format,
			"I", SPA_MEDIA_TYPE_stream,
			"I", SPA_MEDIA_SUBTYPE_midi);
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_buffers(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_pod_builder_object(b,
		SPA_ID_PARAM_Buffers, SPA_ID_OBJECT_ParamBuffers,
		":", SPA_PARAM_BUFFERS_buffers, "iru", 1,
			SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
		":", SPA_PARAM_BUFFERS_blocks,  "i", 1,
		":", SPA_PARAM_BUFFERS_size,    "isu", MAX_BUFFER_SIZE * sizeof(float),
			SPA_POD_PROP_STEP(4, INT32_MAX, 4),
		":", SPA_PARAM_BUFFERS_stride,  "i", 4,
		":", SPA_PARAM_BUFFERS_align,   "i", 16);
	return 1;
}

static int port_set_format(struct client *c, struct port *p,
		uint32_t flags, const struct spa_pod *param)
{
	if (param == NULL) {
		struct mix *mix;

		pw_log_debug(NAME" %p: port %p clear format", c, p);

		spa_list_for_each(mix, &p->mix, port_link)
			clear_buffers(c, mix);
		p->have_format = false;
	}
	else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(param,
			"I", &info.media_type,
			"I", &info.media_subtype);

		switch (info.media_type) {
		case SPA_MEDIA_TYPE_audio:
			if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;

			if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
				return -EINVAL;

			p->rate = info.info.raw.rate;
			break;

		case SPA_MEDIA_TYPE_stream:
			break;
		default:
			return -EINVAL;
		}
		p->have_format = true;
	}
	return 0;
}

static void client_node_port_set_param(void *object,
                                uint32_t seq,
                                enum spa_direction direction,
                                uint32_t port_id,
                                uint32_t id, uint32_t flags,
                                const struct spa_pod *param)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
	struct spa_pod *params[4];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

        if (id == SPA_ID_PARAM_Format) {
		port_set_format(c, p, flags, param);
	}

	param_enum_format(c, p, &params[0], &b);
	param_format(c, p, &params[1], &b);
	param_buffers(c, p, &params[2], &b);

	pw_client_node_proxy_port_update(c->node_proxy,
					 direction,
					 port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS,
					 3,
					 (const struct spa_pod **) params,
					 NULL);

	pw_client_node_proxy_done(c->node_proxy, seq, 0);
}

static void init_buffer(struct port *p, void *data, size_t maxsize)
{
	if (p->object->port.type_id == 1) {
		struct midi_buffer *mb = data;
		mb->magic = MIDI_BUFFER_MAGIC;
		mb->buffer_size = maxsize;
		mb->nframes = maxsize / sizeof(float);
		mb->write_pos = 0;
		mb->event_count = 0;
		mb->lost_events = 0;
	}
	else
		memset(data, 0, maxsize);
}

static void client_node_port_use_buffers(void *object,
                                  uint32_t seq,
                                  enum spa_direction direction,
                                  uint32_t port_id,
                                  uint32_t mix_id,
                                  uint32_t n_buffers,
                                  struct pw_client_node_buffer *buffers)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
	struct buffer *b;
	uint32_t i, j, prot, res;
	struct pw_core *core = c->context.core;
	struct mix *mix;

	if (!p->valid) {
		res = -EINVAL;
		goto done;
	}

	if ((mix = ensure_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto done;
	}

	pw_log_debug(NAME" %p: port %p %d %d.%d use_buffers %d", c, p, direction,
			port_id, mix_id, n_buffers);

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(c, mix);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;
		struct spa_buffer *buf;
		struct mem *m;

		if ((m = find_mem(&c->mems, buffers[i].mem_id)) == NULL) {
			pw_log_warn(NAME" %p: unknown memory id %u", c, buffers[i].mem_id);
			continue;
		}

		buf = buffers[i].buffer;

		b = &mix->buffers[buf->id];
		b->flags = 0;
		b->id = buf->id;
		b->n_mem = 0;

		pw_map_range_init(&b->map, buffers[i].offset, buffers[i].size, core->sc_pagesize);

		b->ptr = mmap(NULL, b->map.size, prot, MAP_SHARED, m->fd, b->map.offset);
		if (b->ptr == MAP_FAILED) {
			b->ptr = NULL;
			pw_log_warn(NAME" %p: Failed to mmap memory %u %u: %m", c,
					b->map.offset, b->map.size);
			continue;
		}


		m->ref++;
		b->mem[b->n_mem++] = m->id;

		pw_log_debug("add buffer %d %d %u %u", m->id, b->id, b->map.offset, b->map.size);

		offset = b->map.start;
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			offset += m->size;
		}

		b->n_datas = SPA_MIN(buf->n_datas, MAX_BUFFER_DATAS);

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buf->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(b->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) {
				struct mem *bm = find_mem(&c->mems, SPA_PTR_TO_UINT32(d->data));

				d->data = mmap(NULL, d->maxsize + d->mapoffset, prot,
							MAP_SHARED, bm->fd, 0);
				if (d->data == MAP_FAILED) {
					pw_log_error(NAME" %p: failed to map buffer mem %m", c);
					d->data = NULL;
					res = -errno;
					goto done;
				}
				d->data = SPA_MEMBER(d->data, d->mapoffset, void);
				d->fd = bm->fd;
				bm->ref++;
				b->mem[b->n_mem++] = bm->id;
				pw_log_debug(NAME" %p: data %d %u -> fd %d %d",
						c, j, bm->id, bm->fd, d->maxsize);
			} else if (d->type == SPA_DATA_MemPtr) {
				d->data = SPA_MEMBER(b->ptr,
						b->map.start + SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(NAME" %p: data %d %u -> mem %p %d",
						c, j, b->id, d->data, d->maxsize);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
			if (mlock(d->data, d->maxsize) < 0)
				pw_log_warn(NAME" %p: Failed to mlock memory %p %u: %m", c,
						d->data, d->maxsize);
		}

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
		if (direction == SPA_DIRECTION_OUTPUT) {
			init_buffer(p, b->datas[0].data, b->datas[0].maxsize);
			reuse_buffer(c, mix, b->id);
		}

	}
	pw_log_debug("have %d buffers", n_buffers);
	mix->n_buffers = n_buffers;
	res = 0;

      done:
	pw_client_node_proxy_done(c->node_proxy, seq, res);
}

static void client_node_port_command(void *object,
                              enum spa_direction direction,
                              uint32_t port_id,
                              const struct spa_command *command)
{
}

static void clear_io(struct client *c, struct io *io)
{
	struct mem *m;
	m = find_mem(&c->mems, io->memid);
	if (m && --m->ref == 0)
		clear_mem(c, m);
	io->id = SPA_ID_INVALID;
}

static struct io *update_io(struct client *c, struct mix *mix,
		uint32_t id, uint32_t memid)
{
	int i;
	struct io *io, *f = NULL;

	for (i = 0; i < MAX_IO; i++) {
		io = &mix->ios[i];
		if (io->id == SPA_ID_INVALID)
			f = io;
		else if (io->id == id) {
			if (io->memid != memid)
				clear_io(c, io);
			f = io;
			break;
		}
	}
	if (f == NULL)
		return NULL;

	io = f;
	io->id = id;
	io->memid = memid;
	return io;
}

static void client_node_port_set_io(void *object,
                             uint32_t seq,
                             enum spa_direction direction,
                             uint32_t port_id,
                             uint32_t mix_id,
                             uint32_t id,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
        struct mem *m;
        struct mix *mix;
        void *ptr;
	int res = 0;

	if ((mix = ensure_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto exit;
	}

        if (mem_id == SPA_ID_INVALID) {
                ptr = NULL;
                size = 0;
        }
        else {
                m = find_mem(&c->mems, mem_id);
                if (m == NULL) {
                        pw_log_warn("unknown memory id %u", mem_id);
			res = -EINVAL;
                        goto exit;
                }
                if ((ptr = mem_map(c, m, offset, size)) == NULL) {
			res = -errno;
                        goto exit;
		}
		m->ref++;
        }

	update_io(c, mix, id, mem_id);

        if (id == SPA_ID_IO_Buffers)
                mix->io = ptr;

	pw_log_debug("port %p: set io id %u %u %u %u %p", p, id, mem_id, offset, size, ptr);

      exit:
	pw_client_node_proxy_done(c->node_proxy, seq, res);
}

static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.add_mem = client_node_add_mem,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.set_io = client_node_set_io,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_command = client_node_port_command,
	.port_set_io = client_node_port_set_io,
};

static jack_port_type_id_t string_to_type(const char *port_type)
{
	if (!strcmp(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return 0;
	else if (!strcmp(JACK_DEFAULT_MIDI_TYPE, port_type))
		return 1;
	else if (!strcmp("other", port_type))
		return 2;
	else
		return SPA_ID_INVALID;
}

static const char* type_to_string(jack_port_type_id_t type_id)
{
	switch(type_id) {
	case 0:
		return JACK_DEFAULT_AUDIO_TYPE;
	case 1:
		return JACK_DEFAULT_MIDI_TYPE;
	case 2:
		return "other";
	default:
		return NULL;
	}
}

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
                                  uint32_t permissions, uint32_t type, uint32_t version,
                                  const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
	struct object *o, *ot;
	const char *str;
	size_t size;

	if (props == NULL)
		return;

	if (type == PW_ID_INTERFACE_Node) {
		if ((str = spa_dict_lookup(props, "node.name")) == NULL)
			goto exit;

		o = alloc_object(c);
		spa_list_append(&c->context.nodes, &o->link);

		snprintf(o->node.name, sizeof(o->node.name), "%s/%d", str, id);
		pw_log_debug("add node %d", id);
	}
	else if (type == PW_ID_INTERFACE_Port) {
		const struct spa_dict_item *item;
		unsigned long flags = 0;
		jack_port_type_id_t type_id;
		char full_name[1024];

		if ((str = spa_dict_lookup(props, "port.dsp")) == NULL) {
			type_id = 2;
		}
		else
			if ((type_id = string_to_type(str)) == SPA_ID_INVALID)
				goto exit;

		if ((str = spa_dict_lookup(props, "port.name")) == NULL)
			goto exit;

		spa_dict_for_each(item, props) {
	                if (!strcmp(item->key, "port.direction")) {
				if (!strcmp(item->value, "in"))
					flags |= JackPortIsInput;
				else if (!strcmp(item->value, "out"))
					flags |= JackPortIsOutput;
			}
			else if (!strcmp(item->key, "port.physical")) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsPhysical;
			}
			else if (!strcmp(item->key, "port.terminal")) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsTerminal;
			}
		}

		o = NULL;
		if (parent_id == c->node_id) {
			snprintf(full_name, sizeof(full_name), "%s:%s", c->name, str);
			o = find_port(c, full_name);
			if (o != NULL)
				pw_log_debug("client %p: %s found our port %p", c, full_name, o);
		}
		if (o == NULL) {
			o = alloc_object(c);
			if (o == NULL)
				goto exit;

			spa_list_append(&c->context.ports, &o->link);
			ot = pw_map_lookup(&c->context.globals, parent_id);
			if (ot == NULL || ot->type != PW_ID_INTERFACE_Node)
				goto exit_free;

			snprintf(o->port.name, sizeof(o->port.name), "%s:%s", ot->node.name, str);
			o->port.port_id = SPA_ID_INVALID;
		}

		if ((str = spa_dict_lookup(props, "port.alias1")) != NULL)
			snprintf(o->port.alias1, sizeof(o->port.alias1), "%s", str);
		else
			o->port.alias1[0] = '\0';

		if ((str = spa_dict_lookup(props, "port.alias2")) != NULL)
			snprintf(o->port.alias2, sizeof(o->port.alias2), "%s", str);
		else
			o->port.alias2[0] = '\0';

		o->port.flags = flags;
		o->port.type_id = type_id;

		pw_log_debug("add port %d %s", id, o->port.name);
	}
	else if (type == PW_ID_INTERFACE_Link) {
		o = alloc_object(c);
		spa_list_append(&c->context.links, &o->link);

		if ((str = spa_dict_lookup(props, "link.output")) == NULL)
			goto exit_free;
		o->port_link.src = pw_properties_parse_int(str);

		if ((str = spa_dict_lookup(props, "link.input")) == NULL)
			goto exit_free;
		o->port_link.dst = pw_properties_parse_int(str);

		pw_log_debug("add link %d %d->%d", id, o->port_link.src, o->port_link.dst);
	}
	else
		goto exit;

	o->type = type;
	o->id = id;
	o->parent_id = parent_id;

        size = pw_map_get_size(&c->context.globals);
        while (id > size)
		pw_map_insert_at(&c->context.globals, size++, NULL);
	pw_map_insert_at(&c->context.globals, id, o);

	if (type == PW_ID_INTERFACE_Node) {
		if (c->registration_callback)
			c->registration_callback(o->node.name, 1, c->registration_arg);
	}
	else if (type == PW_ID_INTERFACE_Port) {
		if (c->portregistration_callback)
			c->portregistration_callback(o->id, 1, c->portregistration_arg);
	}
	else if (type == PW_ID_INTERFACE_Link) {
		if (c->connect_callback)
			c->connect_callback(o->port_link.src, o->port_link.dst, 1, c->connect_arg);
	}

      exit:
	return;
      exit_free:
	free_object(c, o);
	return;
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	struct client *c = (struct client *) object;
	struct object *o;

	pw_log_debug("removed: %u", id);

	o = pw_map_lookup(&c->context.globals, id);
	if (o == NULL)
		return;

	if (o->type == PW_ID_INTERFACE_Node) {
		if (c->registration_callback)
			c->registration_callback(o->node.name, 0, c->registration_arg);
	}
	else if (o->type == PW_ID_INTERFACE_Port) {
		if (c->portregistration_callback)
			c->portregistration_callback(o->id, 0, c->portregistration_arg);
	}
	else if (o->type == PW_ID_INTERFACE_Link) {
		if (c->connect_callback)
			c->connect_callback(o->port_link.src, o->port_link.dst, 0, c->connect_arg);
	}

	/* JACK clients expect the objects to hang around after
	 * they are unregistered. We keep them in the map but reuse the
	 * object when we can
	 * pw_map_insert_at(&c->context.globals, id, NULL);
	 **/
	free_object(c, o);
}

static const struct pw_registry_proxy_events registry_events = {
        PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
};

jack_client_t * jack_client_open (const char *client_name,
                                  jack_options_t options,
                                  jack_status_t *status, ...)
{
	struct client *client;
	bool busy = true;
	struct spa_dict props;
	struct spa_dict_item items[5];
	int i;

	pw_log_debug("client open %s %d", client_name, options);

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto init_failed;

	client->node_id = SPA_ID_INVALID;
	strncpy(client->name, client_name, JACK_CLIENT_NAME_SIZE);
	client->context.main = pw_main_loop_new(NULL);
	client->context.loop = pw_thread_loop_new(pw_main_loop_get_loop(client->context.main), client_name);
        client->context.core = pw_core_new(pw_thread_loop_get_loop(client->context.loop), NULL);
	spa_list_init(&client->context.free_objects);
	spa_list_init(&client->context.nodes);
	spa_list_init(&client->context.ports);
	spa_list_init(&client->context.links);

        pw_array_init(&client->mems, 64);
        pw_array_ensure_size(&client->mems, sizeof(struct mem) * 64);

	client->buffer_size = -1;
	client->sample_rate = -1;

        spa_list_init(&client->mix[SPA_DIRECTION_INPUT]);
        spa_list_init(&client->mix[SPA_DIRECTION_OUTPUT]);
        spa_list_init(&client->free_mix);
	for (i = 0; i < MAX_MIX; i++)
		spa_list_append(&client->free_mix, &client->mix_pool[i].link);

	init_port_pool(client, SPA_DIRECTION_INPUT);
	init_port_pool(client, SPA_DIRECTION_OUTPUT);

	pw_map_init(&client->context.globals, 64, 64);

	pw_thread_loop_start(client->context.loop);

	pw_thread_loop_lock(client->context.loop);
        client->remote = pw_remote_new(client->context.core,
				pw_properties_new(
					"client.name", client_name,
					"client.api", "jack",
					NULL),
				0);

        pw_remote_add_listener(client->remote, &client->remote_listener, &remote_events, client);

        if (pw_remote_connect(client->remote) < 0)
		goto server_failed;

	while (busy) {
		const char *error = NULL;

	        pw_thread_loop_wait(client->context.loop);

		switch (pw_remote_get_state(client->remote, &error)) {
		case PW_REMOTE_STATE_ERROR:
			goto server_failed;

		case PW_REMOTE_STATE_CONNECTED:
			busy = false;
			break;

		default:
			break;
		}
	}
	client->core_proxy = pw_remote_get_core_proxy(client->remote);
	client->registry_proxy = pw_core_proxy_get_registry(client->core_proxy,
						PW_ID_INTERFACE_Registry,
						PW_VERSION_REGISTRY, 0);
	pw_registry_proxy_add_listener(client->registry_proxy,
                                               &client->registry_listener,
                                               &registry_events, client);


	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT("node.name", client_name);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_NODE_PROP_MEDIA, "Audio");
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_NODE_PROP_CATEGORY, "Duplex");
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_NODE_PROP_ROLE, "DSP");
	items[props.n_items++] = SPA_DICT_ITEM_INIT("node.latency", "128/48000");

	client->node_proxy = pw_core_proxy_create_object(client->core_proxy,
				"client-node",
				PW_ID_INTERFACE_ClientNode,
				PW_VERSION_CLIENT_NODE,
				&props,
				0);
	if (client->node_proxy == NULL)
		goto init_failed;

	pw_client_node_proxy_add_listener(client->node_proxy,
			&client->node_listener, &client_node_events, client);
        pw_proxy_add_listener((struct pw_proxy*)client->node_proxy,
			&client->proxy_listener, &proxy_events, client);

	pw_client_node_proxy_update(client->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS,
				    0, 0, 0, NULL, NULL);

	pw_client_node_proxy_done(client->node_proxy, 0, 0);

	if (do_sync(client) < 0)
		goto init_failed;

	pw_thread_loop_unlock(client->context.loop);

	if (status)
		*status = 0;

	return (jack_client_t *)client;

      init_failed:
	if (status)
		*status = JackFailure | JackInitFailure;
	goto exit;
      server_failed:
	if (status)
		*status = JackFailure | JackServerFailed;
	goto exit;
     exit:
	pw_thread_loop_unlock(client->context.loop);
	return NULL;
}

jack_client_t * jack_client_new (const char *client_name)
{
	jack_options_t options = JackUseExactName;
	jack_status_t status;

        if (getenv("JACK_START_SERVER") == NULL)
            options |= JackNoStartServer;

	return jack_client_open(client_name, options, &status, NULL);
}

int jack_client_close (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	pw_log_debug("client %p: close", client);

	pw_thread_loop_stop(c->context.loop);

	pw_core_destroy(c->context.core);
        pw_thread_loop_destroy(c->context.loop);
        pw_main_loop_destroy(c->context.main);
	free(c);

	return 0;
}

int jack_client_name_size (void)
{
	return JACK_CLIENT_NAME_SIZE;
}

char * jack_get_client_name (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return c->name;
}

char *jack_get_uuid_for_client_name (jack_client_t *client,
                                     const char    *client_name)
{
	pw_log_warn("not implemented");
	return NULL;
}

char *jack_get_client_name_by_uuid (jack_client_t *client,
                                    const char    *client_uuid )
{
	pw_log_warn("not implemented");
	return NULL;
}

int jack_internal_client_new (const char *client_name,
                              const char *load_name,
                              const char *load_init)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

void jack_internal_client_close (const char *client_name)
{
	pw_log_warn("not implemented");
}

int jack_activate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);
        pw_client_node_proxy_done(c->node_proxy, 0, 0);
	pw_client_node_proxy_set_active(c->node_proxy, true);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	if (res > 0)
		c->active = true;

	return res;
}

int jack_deactivate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);
        pw_client_node_proxy_done(c->node_proxy, 0, 0);
	pw_client_node_proxy_set_active(c->node_proxy, false);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	if (res > 0)
		c->active = false;

	return res;
}

int jack_get_client_pid (const char *name)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_native_thread_t jack_client_thread_id (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_is_realtime (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_nframes_t jack_thread_wait (jack_client_t *client, int status)
{
	pw_log_warn("not implemented");
	return 0;
}

jack_nframes_t jack_cycle_wait (jack_client_t* client)
{
	pw_log_warn("not implemented");
	return 0;
}

void jack_cycle_signal (jack_client_t* client, int status)
{
	pw_log_warn("not implemented");
}

int jack_set_process_thread(jack_client_t* client, JackThreadCallback thread_callback, void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error("jack %p: can't set callback on active client", c);
		return -EIO;
	} else if (c->process_callback) {
		pw_log_error("jack %p: process callback was already set", c);
		return -EIO;
	}

	c->thread_callback = thread_callback;
	c->thread_arg = arg;
	return 0;
}

int jack_set_thread_init_callback (jack_client_t *client,
                                   JackThreadInitCallback thread_init_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	c->thread_init_callback = thread_init_callback;
	c->thread_init_arg = arg;
	return 0;
}

void jack_on_shutdown (jack_client_t *client,
                       JackShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;
	c->shutdown_callback = shutdown_callback;
	c->shutdown_arg = arg;
}

void jack_on_info_shutdown (jack_client_t *client,
                            JackInfoShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;
	c->info_shutdown_callback = shutdown_callback;
	c->info_shutdown_arg = arg;
}

int jack_set_process_callback (jack_client_t *client,
                               JackProcessCallback process_callback,
                               void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error("jack %p: can't set callback on active client", c);
		return -EIO;
	} else if (c->thread_callback) {
		pw_log_error("jack %p: thread callback was already set", c);
		return -EIO;
	}

	pw_log_debug("jack %p: %p %p", c, process_callback, arg);
	c->process_callback = process_callback;
	c->process_arg = arg;
	return 0;
}

int jack_set_freewheel_callback (jack_client_t *client,
                                 JackFreewheelCallback freewheel_callback,
                                 void *arg)
{
	struct client *c = (struct client *) client;
	c->freewheel_callback = freewheel_callback;
	c->freewheel_arg = arg;
	return 0;
}

int jack_set_buffer_size_callback (jack_client_t *client,
                                   JackBufferSizeCallback bufsize_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	c->bufsize_callback = bufsize_callback;
	c->bufsize_arg = arg;
	return 0;
}

int jack_set_sample_rate_callback (jack_client_t *client,
                                   JackSampleRateCallback srate_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	c->srate_callback = srate_callback;
	c->srate_arg = arg;
	return 0;
}

int jack_set_client_registration_callback (jack_client_t *client,
                                            JackClientRegistrationCallback
                                            registration_callback, void *arg)
{
	struct client *c = (struct client *) client;
	c->registration_callback = registration_callback;
	c->registration_arg = arg;
	return 0;
}

int jack_set_port_registration_callback (jack_client_t *client,
                                          JackPortRegistrationCallback
                                          registration_callback, void *arg)
{
	struct client *c = (struct client *) client;
	c->portregistration_callback = registration_callback;
	c->portregistration_arg = arg;
	return 0;
}


int jack_set_port_connect_callback (jack_client_t *client,
                                    JackPortConnectCallback
                                    connect_callback, void *arg)
{
	struct client *c = (struct client *) client;
	c->connect_callback = connect_callback;
	c->connect_arg = arg;
	return 0;
}

int jack_set_port_rename_callback (jack_client_t *client,
                                   JackPortRenameCallback
                                   rename_callback, void *arg)
{
	return 0;
}

int jack_set_graph_order_callback (jack_client_t *client,
                                   JackGraphOrderCallback graph_callback,
                                   void *data)
{
	struct client *c = (struct client *) client;
	c->graph_callback = graph_callback;
	c->graph_arg = data;
	return 0;
}

int jack_set_xrun_callback (jack_client_t *client,
                            JackXRunCallback xrun_callback, void *arg)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_set_latency_callback (jack_client_t *client,
			       JackLatencyCallback latency_callback,
			       void *data)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_set_freewheel(jack_client_t* client, int onoff)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_set_buffer_size (jack_client_t *client, jack_nframes_t nframes)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_nframes_t jack_get_sample_rate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	if (c->sample_rate == -1)
		return DEFAULT_SAMPLE_RATE;
	return c->sample_rate;
}

jack_nframes_t jack_get_buffer_size (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	if (c->buffer_size == -1)
		return DEFAULT_BUFFER_SIZE;
	return c->buffer_size;
}

int jack_engine_takeover_timebase (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

float jack_cpu_load (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return 0.0;
}

jack_port_t * jack_port_register (jack_client_t *client,
                                  const char *port_name,
                                  const char *port_type,
                                  unsigned long flags,
                                  unsigned long buffer_size)
{
	struct client *c = (struct client *) client;
	enum spa_direction direction;
	struct spa_port_info port_info = { 0, };
	struct spa_dict dict;
	struct spa_dict_item items[10];
	struct object *o;
	jack_port_type_id_t type_id;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *params[4];
	struct port *p;
	int res;

	pw_log_debug("client %p: port register \"%s\" \"%s\" %ld %ld",
			c, port_name, port_type, flags, buffer_size);

	if (flags & JackPortIsInput)
		direction = PW_DIRECTION_INPUT;
	else if (flags & JackPortIsOutput)
		direction = PW_DIRECTION_OUTPUT;
	else
		return NULL;

	if ((type_id = string_to_type(port_type)) == SPA_ID_INVALID)
		return NULL;

	if ((p = alloc_port(c, direction)) == NULL)
		return NULL;

	o = p->object;
	o->port.flags = flags;
	snprintf(o->port.name, sizeof(o->port.name), "%s:%s", c->name, port_name);
	o->port.type_id = type_id;

	spa_list_init(&p->mix);

	port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			  SPA_PORT_INFO_FLAG_NO_REF;

	port_info.props = &dict;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT("port.dsp", port_type);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT("port.name", port_name);

	param_enum_format(c, p, &params[0], &b);
	param_buffers(c, p, &params[1], &b);

	pw_thread_loop_lock(c->context.loop);

	pw_client_node_proxy_port_update(c->node_proxy,
					 direction,
					 p->id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO ,
					 2,
					 (const struct spa_pod **) params,
					 &port_info);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	if (res < 0)
		return NULL;

	return (jack_port_t *) o;
}

int jack_port_unregister (jack_client_t *client, jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct port *p;
	int res;

	if (o->type != PW_ID_INTERFACE_Port || o->port.port_id == SPA_ID_INVALID) {
		pw_log_error("client %p: invalid port %p", client, port);
		return -EINVAL;
	}
	pw_log_debug("client %p: port unregister %p", client, port);

	pw_thread_loop_lock(c->context.loop);

	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	free_port(c, p);

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 0, 0, NULL, NULL);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	return res;
}

static void add_f32(float *out, float *in1, float *in2, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		out[i] = in1[i] + in2[i];
}

void * jack_port_get_buffer (jack_port_t *port, jack_nframes_t frames)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct port *p;
	struct buffer *b;
	struct spa_io_buffers *io;
	struct mix *mix;
	int layer = 0;
	void *ptr = NULL;

	if (o->type != PW_ID_INTERFACE_Port || o->port.port_id == SPA_ID_INVALID) {
		pw_log_error("client %p: invalid port %p", c, port);
		return NULL;
	}
	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	if (p->direction == SPA_DIRECTION_INPUT) {
		spa_list_for_each(mix, &p->mix, port_link) {
			pw_log_trace("port %p: mix %d.%d get buffer %d",
					p, p->id, mix->id, frames);
			io = mix->io;
			if (io == NULL || io->buffer_id >= mix->n_buffers)
				continue;

			io->status = SPA_STATUS_NEED_BUFFER;
			b = &mix->buffers[io->buffer_id];
			if (layer++ == 0)
				ptr = b->datas[0].data;
			else  {
				add_f32(p->empty, ptr, b->datas[0].data, frames);
				ptr = p->empty;
				p->zeroed = false;
			}
		}
	} else {
		b = NULL;
		spa_list_for_each(mix, &p->mix, port_link) {
			pw_log_trace("port %p: mix %d.%d get buffer %d",
					p, p->id, mix->id, frames);
			io = mix->io;

			if (mix->n_buffers == 0 || io == NULL)
				continue;

			if ((b = dequeue_buffer(mix)) == NULL) {
				pw_log_warn("port %p: out of buffers", p);
				io->buffer_id = SPA_ID_INVALID;
				goto done;
			}
			reuse_buffer(c, mix, b->id);
			ptr = b->datas[0].data;

			b->datas[0].chunk->offset = 0;
			b->datas[0].chunk->size = frames * sizeof(float);
			b->datas[0].chunk->stride = sizeof(float);

			io->status = SPA_STATUS_HAVE_BUFFER;
			io->buffer_id = b->id;
			break;
		}
		spa_list_for_each(mix, &p->mix, port_link) {
			struct spa_io_buffers *mio = mix->io;
			if (mio == NULL)
				continue;
			*mio = *io;
		}
	}

      done:
	if (ptr == NULL) {
		ptr = p->empty;
		if (!p->zeroed) {
			init_buffer(p, ptr, sizeof(p->empty));
			p->zeroed = true;
		}
	}
	pw_log_trace("port %p: buffer %p", p, ptr);
	return ptr;
}

jack_uuid_t jack_port_uuid (const jack_port_t *port)
{
	pw_log_warn("not implemented");
	return 0;
}

const char * jack_port_name (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.name;
}

const char * jack_port_short_name (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return strchr(o->port.name, ':') + 1;
}

int jack_port_flags (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.flags;
}

const char * jack_port_type (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return type_to_string(o->port.type_id);
}

jack_port_type_id_t jack_port_type_id (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.type_id;
}

int jack_port_is_mine (const jack_client_t *client, const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->type == PW_ID_INTERFACE_Port && o->port.port_id != SPA_ID_INVALID;
}

int jack_port_connected (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct object *l;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);
	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id ||
		    l->port_link.dst == o->id)
			res++;
	}
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

int jack_port_connected_to (const jack_port_t *port,
                            const char *port_name)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct object *p, *l;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);

	p = find_port(c, port_name);
	if (p == NULL)
		goto exit;

	if (GET_DIRECTION(p->port.flags) == GET_DIRECTION(o->port.flags))
		goto exit;

	if (p->port.flags & JackPortIsOutput) {
		l = p;
		p = o;
		o = l;
	}
	if (find_link(c, o->id, p->id))
		res = 1;

     exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

const char ** jack_port_get_connections (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;

	return jack_port_get_all_connections((jack_client_t *)c, port);
}

const char ** jack_port_get_all_connections (const jack_client_t *client,
                                             const jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = (struct object *) port;
	struct object *p, *l;
	const char **res = malloc(sizeof(char*) * (CONNECTION_NUM_FOR_PORT + 1));
	int count = 0;

	pw_thread_loop_lock(c->context.loop);

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id)
			p = pw_map_lookup(&c->context.globals, l->port_link.dst);
		else if (l->port_link.dst == o->id)
			p = pw_map_lookup(&c->context.globals, l->port_link.src);
		else
			continue;

		if (p == NULL)
			continue;

		res[count++] = p->port.name;
		if (count == CONNECTION_NUM_FOR_PORT)
			break;
	}
	pw_thread_loop_unlock(c->context.loop);

	if (count == 0) {
		free(res);
		res = NULL;
	} else
		res[count] = NULL;

	return res;
}

int jack_port_tie (jack_port_t *src, jack_port_t *dst)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_untie (jack_port_t *port)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_set_name (jack_port_t *port, const char *port_name)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_set_alias (jack_port_t *port, const char *alias)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_unset_alias (jack_port_t *port, const char *alias)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_get_aliases (const jack_port_t *port, char* const aliases[2])
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);

	if (o->port.alias1[0] != '\0') {
		snprintf(aliases[0], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias1);
		res++;
	}
	if (o->port.alias2[0] != '\0') {
		snprintf(aliases[1], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias2);
		res++;
	}
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

int jack_port_request_monitor (jack_port_t *port, int onoff)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_request_monitor_by_name (jack_client_t *client,
                                       const char *port_name, int onoff)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_ensure_monitor (jack_port_t *port, int onoff)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_port_monitoring_input (jack_port_t *port)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_connect (jack_client_t *client,
                  const char *source_port,
                  const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst;
	struct spa_dict props;
	struct spa_dict_item items[4];
	char val[4][16];
	int res;

	pw_log_debug("client %p: connect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);

	src = find_port(c, source_port);
	dst = find_port(c, destination_port);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput)) {
		res = -EINVAL;
		goto exit;
	}

	snprintf(val[0], sizeof(val[0]), "%d", src->parent_id);
	snprintf(val[1], sizeof(val[1]), "%d", src->id);
	snprintf(val[2], sizeof(val[2]), "%d", dst->parent_id);
	snprintf(val[3], sizeof(val[3]), "%d", dst->id);

	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_LINK_OUTPUT_NODE_ID, val[0]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_LINK_OUTPUT_PORT_ID, val[1]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_LINK_INPUT_NODE_ID, val[2]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_LINK_INPUT_PORT_ID, val[3]);

	pw_core_proxy_create_object(c->core_proxy,
				    "link-factory",
				    PW_ID_INTERFACE_Link,
				    PW_VERSION_LINK,
				    &props,
				    0);
	res = do_sync(c);

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

int jack_disconnect (jack_client_t *client,
                     const char *source_port,
                     const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst, *l;
	int res;

	pw_log_debug("client %p: disconnect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);

	src = find_port(c, source_port);
	dst = find_port(c, destination_port);

	pw_log_debug("client %p: %d %d", client, src->id, dst->id);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput)) {
		res = -EINVAL;
		goto exit;
	}

	if ((l = find_link(c, src->id, dst->id)) == NULL) {
		res = -ENOENT;
		goto exit;
	}

	pw_core_proxy_destroy(c->core_proxy, l->id);

	res = do_sync(c);

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

int jack_port_disconnect (jack_client_t *client, jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = (struct object *) port;
	struct object *l;
	int res;

	pw_log_debug("client %p: disconnect %p", client, port);

	pw_thread_loop_lock(c->context.loop);

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id ||
		    l->port_link.dst == o->id) {
			pw_core_proxy_destroy(c->core_proxy, l->id);
		}
	}
	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	return res;
}

int jack_port_name_size(void)
{
	return REAL_JACK_PORT_NAME_SIZE+1;
}

int jack_port_type_size(void)
{
	return JACK_PORT_TYPE_SIZE+1;
}

size_t jack_port_type_get_buffer_size (jack_client_t *client, const char *port_type)
{
	struct client *c = (struct client *) client;
	if (!strcmp(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return c->buffer_size;
	else if (!strcmp(JACK_DEFAULT_MIDI_TYPE, port_type))
		return BUFFER_SIZE_MAX;
	else
		return 0;
}

void jack_port_set_latency (jack_port_t *port, jack_nframes_t frames)
{
	pw_log_warn("not implemented");
}

void jack_port_get_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	pw_log_warn("not implemented");
}

void jack_port_set_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	pw_log_warn("not implemented");
}

int jack_recompute_total_latencies (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_nframes_t jack_port_get_latency (jack_port_t *port)
{
	pw_log_warn("not implemented");
	return 0;
}

jack_nframes_t jack_port_get_total_latency (jack_client_t *client,
					    jack_port_t *port)
{
	pw_log_warn("not implemented");
	return 0;
}

int jack_recompute_total_latency (jack_client_t *client, jack_port_t* port)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

const char ** jack_get_ports (jack_client_t *client,
                              const char *port_name_pattern,
                              const char *type_name_pattern,
                              unsigned long flags)
{
	struct client *c = (struct client *) client;
	const char **res = malloc(sizeof(char*) * (JACK_PORT_MAX + 1));
	int count = 0;
	struct object *o;
	const char *str;
	uint32_t id;
	regex_t port_regex, type_regex;

	if ((str = getenv("PIPEWIRE_NODE")) != NULL)
		id = pw_properties_parse_int(str);
	else
		id = SPA_ID_INVALID;

	if (port_name_pattern && port_name_pattern[0])
		regcomp(&port_regex, port_name_pattern, REG_EXTENDED | REG_NOSUB);
	if (type_name_pattern && type_name_pattern[0])
		regcomp(&type_regex, type_name_pattern, REG_EXTENDED | REG_NOSUB);

	pw_thread_loop_lock(c->context.loop);

	pw_log_debug("ports %d %s %s %08lx", id, port_name_pattern, type_name_pattern, flags);

	spa_list_for_each(o, &c->context.ports, link) {
		pw_log_debug("check port %s %d %lu",
				o->port.name, o->port.type_id, o->port.flags);
		if (o->port.type_id == 2)
			continue;
		if (!SPA_FLAG_CHECK(o->port.flags, flags))
			continue;
		if (id != SPA_ID_INVALID && o->parent_id != id)
			continue;

		if (port_name_pattern && port_name_pattern[0]) {
			if (regexec(&port_regex, o->port.name, 0, NULL, 0) == REG_NOMATCH)
				continue;
		}
		if (type_name_pattern && type_name_pattern[0]) {
			if (regexec(&type_regex, type_to_string(o->port.type_id),
						0, NULL, 0) == REG_NOMATCH)
				continue;
		}

		pw_log_debug("add port %d %s", count, o->port.name);
		res[count++] = o->port.name;
		if (count == JACK_PORT_MAX)
			break;
	}
	pw_thread_loop_unlock(c->context.loop);

	if (count == 0) {
		free(res);
		res = NULL;
	} else
		res[count] = NULL;

	if (port_name_pattern && port_name_pattern[0])
		regfree(&port_regex);
	if (type_name_pattern && type_name_pattern[0])
		regfree(&type_regex);

	return res;
}

jack_port_t * jack_port_by_name (jack_client_t *client, const char *port_name)
{
	struct client *c = (struct client *) client;
	struct object *res;

	pw_thread_loop_lock(c->context.loop);

	res = find_port(c, port_name);

	pw_thread_loop_unlock(c->context.loop);

	return (jack_port_t *)res;
}

jack_port_t * jack_port_by_id (jack_client_t *client,
                               jack_port_id_t port_id)
{
	struct client *c = (struct client *) client;
	struct object *res = NULL, *o;

	pw_thread_loop_lock(c->context.loop);

	o = pw_map_lookup(&c->context.globals, port_id);
	pw_log_debug("client %p: port %d -> %p", c, port_id, o);

	if (o == NULL || o->type != PW_ID_INTERFACE_Port)
		goto exit;

	res = o;

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return (jack_port_t *)res;
}

jack_nframes_t jack_frames_since_cycle_start (const jack_client_t *client)
{
	pw_log_warn("not implemented");
	return 0;
}

jack_nframes_t jack_frame_time (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return c->jack_position.frame;
}

jack_nframes_t jack_last_frame_time (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return c->jack_position.frame;
}

int jack_get_cycle_times(const jack_client_t *client,
                        jack_nframes_t *current_frames,
                        jack_time_t    *current_usecs,
                        jack_time_t    *next_usecs,
                        float          *period_usecs)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_time_t jack_frames_to_time(const jack_client_t *client, jack_nframes_t frames)
{
	pw_log_warn("not implemented");
	return 0;
}

jack_nframes_t jack_time_to_frames(const jack_client_t *client, jack_time_t usecs)
{
	pw_log_warn("not implemented");
	return 0;
}

jack_time_t jack_get_time()
{
	pw_log_warn("not implemented");
	return 0;
}

void jack_set_error_function (void (*func)(const char *))
{
	pw_log_warn("not implemented");
}

void jack_set_info_function (void (*func)(const char *))
{
	pw_log_warn("not implemented");
}

void jack_free(void* ptr)
{
	free(ptr);
}

int jack_release_timebase (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_set_sync_callback (jack_client_t *client,
			    JackSyncCallback sync_callback,
			    void *arg)
{
	struct client *c = (struct client *) client;
	c->sync_callback = sync_callback;
	c->sync_arg = arg;
	return 0;
}

int jack_set_sync_timeout (jack_client_t *client,
			   jack_time_t timeout)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int  jack_set_timebase_callback (jack_client_t *client,
				 int conditional,
				 JackTimebaseCallback timebase_callback,
				 void *arg)
{
	struct client *c = (struct client *) client;
	c->timebase_callback = timebase_callback;
	c->timebase_arg = arg;
	return 0;
}

int  jack_transport_locate (jack_client_t *client,
			    jack_nframes_t frame)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

jack_transport_state_t jack_transport_query (const jack_client_t *client,
					     jack_position_t *pos)
{
	struct client *c = (struct client *) client;
	if (pos != NULL)
		memcpy(pos, &c->jack_position, sizeof(jack_position_t));
	return JackTransportRolling;
}

jack_nframes_t jack_get_current_transport_frame (const jack_client_t *client)
{
	pw_log_warn("not implemented");
	return 0;
}

int  jack_transport_reposition (jack_client_t *client,
				const jack_position_t *pos)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

void jack_transport_start (jack_client_t *client)
{
	pw_log_warn("not implemented");
}

void jack_transport_stop (jack_client_t *client)
{
	pw_log_warn("not implemented");
}

void jack_get_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	static jack_transport_info_t dummy;
	memcpy(tinfo, &dummy, sizeof(jack_transport_info_t));
	pw_log_warn("not implemented");
}

void jack_set_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	pw_log_warn("not implemented");
}

int jack_set_session_callback (jack_client_t       *client,
                               JackSessionCallback  session_callback,
                               void                *arg)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_session_reply (jack_client_t        *client,
                        jack_session_event_t *event)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}


void jack_session_event_free (jack_session_event_t *event)
{
	pw_log_warn("not implemented");
}

char *jack_client_get_uuid (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return "";
}

int jack_client_real_time_priority (jack_client_t * client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_client_max_real_time_priority (jack_client_t *client)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_acquire_real_time_scheduling (jack_native_thread_t thread, int priority)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

/**
 * Create a thread for JACK or one of its clients.  The thread is
 * created executing @a start_routine with @a arg as its sole
 * argument.
 *
 * @param client the JACK client for whom the thread is being created. May be
 * NULL if the client is being created within the JACK server.
 * @param thread place to return POSIX thread ID.
 * @param priority thread priority, if realtime.
 * @param realtime true for the thread to use realtime scheduling.  On
 * some systems that may require special privileges.
 * @param start_routine function the thread calls when it starts.
 * @param arg parameter passed to the @a start_routine.
 *
 * @returns 0, if successful; otherwise some error number.
 */
int jack_client_create_thread (jack_client_t* client,
                               jack_native_thread_t *thread,
                               int priority,
                               int realtime, 	/* boolean */
                               void *(*start_routine)(void*),
                               void *arg)
{
	if (globals.creator == NULL)
		globals.creator = pthread_create;

	return globals.creator(thread, NULL, start_routine, arg);
}

int jack_drop_real_time_scheduling (jack_native_thread_t thread)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_client_stop_thread(jack_client_t* client, jack_native_thread_t thread)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

int jack_client_kill_thread(jack_client_t* client, jack_native_thread_t thread)
{
	pw_log_warn("not implemented");
	return -ENOTSUP;
}

void jack_set_thread_creator (jack_thread_creator_t creator)
{
	if (creator == NULL)
		globals.creator = pthread_create;
	else
		globals.creator = creator;
}

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	pw_init(NULL, NULL);
}
