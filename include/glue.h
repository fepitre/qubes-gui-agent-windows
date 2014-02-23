/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

int peer_server_init(
	int port
);
int read_ready_vchan_ext(
);
int read_all_vchan_ext(
	void *buf,
	int size
);
int write_all_vchan_ext(
	void *buf,
	int size
);
int buffer_space_vchan_ext(
);

enum
{
	WRITE_STDIN_OK = 0x200,
	WRITE_STDIN_BUFFERED,
	WRITE_STDIN_ERROR
};

int flush_client_data(
	int fd,
	int client_id,
	struct buffer *buffer
);
int write_stdin(
	int fd,
	int client_id,
	char *data,
	int len,
	struct buffer *buffer
);
void set_nonblock(
	int fd
);
int fork_and_flush_stdin(
	int fd,
	struct buffer *buffer
);

extern struct libvchan *ctrl;

int real_write_message(
	char *hdr,
	int size,
	char *data,
	int datasize
);

#define write_message(x,y) do {\
	x.untrusted_len = sizeof(x); \
	real_write_message((char*)&x, sizeof(x), (char*)&y, sizeof(y)); \
    } while(0)
#define write_data(x, y) write_all_vchan_ext(x, y)
#define write_struct(x) write_data((char*)&x, sizeof(x))

extern WORD X11ToVk[256];

/* From X.h */
#define KeyPress            2
#define ButtonPress            4
#define Button1                 1
#define Button2                 2
#define Button3                 3
#define Button4                 4
#define Button5                 5

#define ShiftMapIndex           0
#define LockMapIndex            1
#define ControlMapIndex         2
#define Mod1MapIndex            3
#define Mod2MapIndex            4
#define Mod3MapIndex            5
#define Mod4MapIndex            6
#define Mod5MapIndex            7