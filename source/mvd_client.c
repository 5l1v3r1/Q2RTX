/*
Copyright (C) 2003-2008 Andrey Nazarov

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
// mvd_client.c -- MVD/GTV client
//

#include "sv_local.h"
#include "mvd_local.h"
#include "mvd_gtv.h"
#include <setjmp.h>

#define GTV_DEFAULT_BACKOFF (5*1000)        // 5 seconds
#define GTV_MAXIMUM_BACKOFF (5*3600*1000)   // 5 hours

#define GTV_PING_INTERVAL   (60*1000)       // 1 minute

typedef enum {
    GTV_DISCONNECTED, // disconnected
    GTV_CONNECTING, // connect() in progress
    GTV_PREPARING,  // waiting for server hello
    GTV_CONNECTED,  // keeping connection alive
    GTV_RESUMING,   // stream start request sent
    GTV_WAITING,    // server is suspended
    GTV_READING,    // server is resumed
    GTV_SUSPENDING, // stream stop request sent
    GTV_NUM_STATES
} gtv_state_t;

typedef struct gtv_s {
    list_t      entry;

    int         id;
    char        name[MAX_MVD_NAME];
    gtv_state_t state;
    mvd_t       *mvd;
    char        *username, *password;

    // connection variables
    netstream_t stream;
    char        address[MAX_QPATH];
    byte        *data;
    size_t      msglen;
    unsigned    flags;
#if USE_ZLIB
    qboolean    z_act; // true when actively inflating
    z_stream    z_str;
    fifo_t      z_buf;
#endif
    unsigned    last_rcvd;
    unsigned    last_sent;
    void        (*drop)( struct gtv_s * );
    void        (*destroy)( struct gtv_s * );
    void        (*run)( struct gtv_s * );
    unsigned    retry_time;
    unsigned    retry_backoff;

    // demo related variables
    fileHandle_t    demoplayback;
    int             demoloop;
    string_entry_t  *demohead, *demoentry;
} gtv_t;

static const char *const gtv_states[GTV_NUM_STATES] = {
    "disconnected",
    "connecting",
    "preparing",
    "connected",
    "resuming",
    "waiting",
    "reading",
    "suspending"
};

static const char *const mvd_states[MVD_NUM_STATES] = {
    "DEAD", "WAIT", "READ"
};

static LIST_DECL( mvd_gtv_list );

LIST_DECL( mvd_channel_list );
LIST_DECL( mvd_active_list );

mvd_t       mvd_waitingRoom;
qboolean    mvd_dirty;
int         mvd_chanid;

jmp_buf     mvd_jmpbuf;

cvar_t      *mvd_shownet;

static qboolean mvd_active;

static cvar_t  *mvd_timeout;
static cvar_t  *mvd_suspend_time;
static cvar_t  *mvd_wait_delay;
static cvar_t  *mvd_wait_percent;
static cvar_t  *mvd_buffer_size;
static cvar_t  *mvd_username;
static cvar_t  *mvd_password;

// ====================================================================


void MVD_Free( mvd_t *mvd ) {
    int i;

    // destroy any existing connection
    if( mvd->gtv ) {
        mvd->gtv->destroy( mvd->gtv );
    }

    // stop demo recording
    if( mvd->demorecording ) {
        uint16_t msglen = 0;
        FS_Write( &msglen, 2, mvd->demorecording );
        FS_FCloseFile( mvd->demorecording );
        mvd->demorecording = 0;
    }

    for( i = 0; i < mvd->maxclients; i++ ) {
        MVD_FreePlayer( &mvd->players[i] );
    }
    Z_Free( mvd->players );

    CM_FreeMap( &mvd->cm );

    Z_Free( mvd->delay.data );

    List_Remove( &mvd->active );
    List_Remove( &mvd->entry );
    Z_Free( mvd );
}

void MVD_Destroy( mvd_t *mvd ) {
    mvd_client_t *client, *next;

    // update channel menus
    if( !LIST_EMPTY( &mvd->active ) ) {
        mvd_dirty = qtrue;
    }

    // cause UDP clients to reconnect
    LIST_FOR_EACH_SAFE( mvd_client_t, client, next, &mvd->clients, entry ) {
        MVD_SwitchChannel( client, &mvd_waitingRoom );
    }

    // free all channel data
    MVD_Free( mvd );
}

void MVD_Destroyf( mvd_t *mvd, const char *fmt, ... ) {
    va_list     argptr;
    char        text[MAXPRINTMSG];

    va_start( argptr, fmt );
    Q_vsnprintf( text, sizeof( text ), fmt, argptr );
    va_end( argptr );

    Com_Printf( "[%s] =X= %s\n", mvd->name, text );

    MVD_Destroy( mvd );

    longjmp( mvd_jmpbuf, -1 );
}

mvd_t *MVD_SetChannel( int arg ) {
    char *s = Cmd_Argv( arg );
    mvd_t *mvd;
    int id;

    if( LIST_EMPTY( &mvd_channel_list ) ) {
        Com_Printf( "No active channels.\n" );
        return NULL;
    }

    if( !*s ) {
        if( List_Count( &mvd_channel_list ) == 1 ) {
            return LIST_FIRST( mvd_t, &mvd_channel_list, entry );
        }
        Com_Printf( "Please specify an exact channel ID.\n" );
        return NULL;
    }

    // special value of @@ returns the channel local client is on
    if( !Com_IsDedicated() && !strcmp( s, "@@" ) ) {
        LIST_FOR_EACH( mvd_t, mvd, &mvd_channel_list, entry ) {
            mvd_client_t *client;

            LIST_FOR_EACH( mvd_client_t, client, &mvd->clients, entry ) {
                if( NET_IsLocalAddress( &client->cl->netchan->remote_address ) ) {
                    return mvd;
                }
            }
        }
    } else if( COM_IsUint( s ) ) {
        id = atoi( s );
        LIST_FOR_EACH( mvd_t, mvd, &mvd_channel_list, entry ) {
            if( mvd->id == id ) {
                return mvd;
            }
        }
    } else {
        LIST_FOR_EACH( mvd_t, mvd, &mvd_channel_list, entry ) {
            if( !strcmp( mvd->name, s ) ) {
                return mvd;
            }
        }
    }

    Com_Printf( "No such channel ID: %s\n", s );
    return NULL;
}

void MVD_CheckActive( mvd_t *mvd ) {
    mvd_t *cur;

    if( mvd->state == MVD_READING || ( mvd->gtv &&
            mvd->gtv->state == GTV_READING ) )
    {
        if( LIST_EMPTY( &mvd->active ) ) {
            // sort this one into the list of active channels
            LIST_FOR_EACH( mvd_t, cur, &mvd_active_list, active ) {
                if( cur->id > mvd->id ) {
                    break;
                }
            }
            List_Append( &cur->active, &mvd->active );
            mvd_dirty = qtrue;
        }
    } else {
        if( !LIST_EMPTY( &mvd->active ) ) {
            // delete this one from the list of active channels
            List_Delete( &mvd->active );
            mvd_dirty = qtrue;
        }
    }
}


/*
====================================================================

COMMON GTV STUFF

====================================================================
*/

static void q_noreturn q_printf( 2, 3 )
gtv_dropf( gtv_t *gtv, const char *fmt, ... ) {
    va_list     argptr;
    char        text[MAXPRINTMSG];

    va_start( argptr, fmt );
    Q_vsnprintf( text, sizeof( text ), fmt, argptr );
    va_end( argptr );

    Com_Printf( "[%s] =!= %s\n", gtv->name, text );

    gtv->drop( gtv );

    longjmp( mvd_jmpbuf, -1 );
}

static void q_noreturn q_printf( 2, 3 )
gtv_destroyf( gtv_t *gtv, const char *fmt, ... ) {
    va_list     argptr;
    char        text[MAXPRINTMSG];

    va_start( argptr, fmt );
    Q_vsnprintf( text, sizeof( text ), fmt, argptr );
    va_end( argptr );

    Com_Printf( "[%s] =X= %s\n", gtv->name, text );

    gtv->destroy( gtv );

    longjmp( mvd_jmpbuf, -1 );
}

static mvd_t *create_channel( gtv_t *gtv ) {
    mvd_t *mvd;

    mvd = MVD_Mallocz( sizeof( *mvd ) );
    mvd->gtv = gtv;
    mvd->id = gtv->id;
    Q_strlcpy( mvd->name, gtv->name, sizeof( mvd->name ) );
    mvd->pool.edicts = mvd->edicts;
    mvd->pool.edict_size = sizeof( edict_t );
    mvd->pool.max_edicts = MAX_EDICTS;
    mvd->pm_type = PM_SPECTATOR;
    mvd->min_packets = mvd_wait_delay->value * 10;
    List_Init( &mvd->clients );
    List_Init( &mvd->entry );
    List_Init( &mvd->active );

    return mvd;
}

static gtv_t *gtv_set_conn( int arg ) {
    char *s = Cmd_Argv( arg );
    gtv_t *gtv;
    int id;

    if( LIST_EMPTY( &mvd_gtv_list ) ) {
        Com_Printf( "No GTV connections.\n" );
        return NULL;
    }

    if( !*s ) {
        if( List_Count( &mvd_gtv_list ) == 1 ) {
            return LIST_FIRST( gtv_t, &mvd_gtv_list, entry );
        }
        Com_Printf( "Please specify an exact connection ID.\n" );
        return NULL;
    }

    if( COM_IsUint( s ) ) {
        id = atoi( s );
        LIST_FOR_EACH( gtv_t, gtv, &mvd_gtv_list, entry ) {
            if( gtv->id == id ) {
                return gtv;
            }
        }
    } else {
        LIST_FOR_EACH( gtv_t, gtv, &mvd_gtv_list, entry ) {
            if( !strcmp( gtv->name, s ) ) {
                return gtv;
            }
        }
    }

    Com_Printf( "No such connection ID: %s\n", s );
    return NULL;
}

/*
==============
MVD_Frame

Called from main server loop.
==============
*/
int MVD_Frame( void ) {
    static unsigned prevtime;
    gtv_t *gtv, *next;
    int connections = 0;

    if( sv.state == ss_broadcast ) {
        unsigned delta = mvd_suspend_time->value * 60 * 1000;

        if( !delta || !LIST_EMPTY( &svs.udp_client_list ) ) {
            prevtime = svs.realtime;
            if( !mvd_active ) {
                Com_DPrintf( "Resuming MVD streams.\n" );
                mvd_active = qtrue;
            }
        } else if( svs.realtime - prevtime > delta ) {
            if( mvd_active ) {
                Com_DPrintf( "Suspending MVD streams.\n" );
                mvd_active = qfalse;
            }
        }
    }

    // run all GTV connections (but not demos)
    LIST_FOR_EACH_SAFE( gtv_t, gtv, next, &mvd_gtv_list, entry ) {
        if( setjmp( mvd_jmpbuf ) ) {
            continue;
        }

        gtv->run( gtv );

        connections++;
    }

    return connections;
}


/*
====================================================================

DEMO PLAYER

====================================================================
*/

static void demo_play_next( gtv_t *gtv, string_entry_t *entry );

static qboolean demo_read_message( fileHandle_t f ) {
    size_t ret;
    uint16_t msglen;

    ret = FS_Read( &msglen, 2, f );
    if( ret != 2 ) {
        return qfalse;
    }
    if( !msglen ) {
        return qfalse;
    }
    msglen = LittleShort( msglen );
    if( msglen > MAX_MSGLEN ) {
        return qfalse;
    }
    ret = FS_Read( msg_read_buffer, msglen, f );
    if( ret != msglen ) {
        return qfalse;
    }

    SZ_Init( &msg_read, msg_read_buffer, sizeof( msg_read_buffer ) );
    msg_read.cursize = msglen;

    return qtrue;
}

static qboolean demo_read_frame( mvd_t *mvd ) {
    gtv_t *gtv = mvd->gtv;

    if( mvd->state == MVD_WAITING ) {
        return qfalse; // paused by user
    }
    if( !gtv ) {
        MVD_Destroyf( mvd, "End of MVD stream reached" );
    }

    if( !demo_read_message( gtv->demoplayback ) ) {
        demo_play_next( gtv, gtv->demoentry->next );
        return qtrue;
    }

    MVD_ParseMessage( mvd );
    return qtrue;
}

static void demo_play_next( gtv_t *gtv, string_entry_t *entry ) {
    uint32_t magic = 0;

    if( !entry ) {
        if( gtv->demoloop ) {
            if( --gtv->demoloop == 0 ) {
                gtv_destroyf( gtv, "End of play list reached" );
            }
        }
        entry = gtv->demohead;
    }

    // close previous file
    if( gtv->demoplayback ) {
        FS_FCloseFile( gtv->demoplayback );
        gtv->demoplayback = 0;
    }

    // open new file
    FS_FOpenFile( entry->string, &gtv->demoplayback, FS_MODE_READ );
    if( !gtv->demoplayback ) {
        gtv_destroyf( gtv, "Couldn't reopen %s", entry->string );
    }

    // figure out if file is compressed and check magic
    if( FS_Read( &magic, 4, gtv->demoplayback ) != 4 ) {
        gtv_destroyf( gtv, "Couldn't read magic from %s", entry->string );
    }
    if( ( ( LittleLong( magic ) & 0xe0ffffff ) == 0x00088b1f ) ) {
        if( !FS_FilterFile( gtv->demoplayback ) ) {
            gtv_destroyf( gtv, "Couldn't install gzip filter on %s", entry->string );
        }
        if( FS_Read( &magic, 4, gtv->demoplayback ) != 4 ) {
            gtv_destroyf( gtv, "Couldn't read magic from %s", entry->string );
        }
    }
    if( magic != MVD_MAGIC ) {
        gtv_destroyf( gtv, "%s is not a MVD2 file", entry->string );
    }

    // read the first message
    if( !demo_read_message( gtv->demoplayback ) ) {
        gtv_destroyf( gtv, "Couldn't read first message from %s", entry->string );
    }

    // create MVD channel
    if( !gtv->mvd ) {
        gtv->mvd = create_channel( gtv );
        gtv->mvd->read_frame = demo_read_frame;
    }

    // parse gamestate
    MVD_ParseMessage( gtv->mvd );
    if( !gtv->mvd->state ) {
        gtv_destroyf( gtv, "First message of %s does not contain gamestate", entry->string );
    }

    gtv->mvd->state = MVD_READING;

    Com_Printf( "[%s] Reading from %s\n", gtv->name, entry->string );

    // reset state
    gtv->demoentry = entry;

    // set channel address
    Q_strlcpy( gtv->address, COM_SkipPath( entry->string ), sizeof( gtv->address ) );
}

static void demo_free_playlist( gtv_t *gtv ) {
    string_entry_t *entry, *next;

    for( entry = gtv->demohead; entry; entry = next ) {
        next = entry->next;
        Z_Free( entry );
    }

    gtv->demohead = gtv->demoentry = NULL;
}

static void demo_destroy( gtv_t *gtv ) {
    mvd_t *mvd = gtv->mvd;

    if( mvd ) {
        mvd->gtv = NULL;
        if( !mvd->state ) {
            MVD_Free( mvd );
        }
    }

    if( gtv->demoplayback ) {
        FS_FCloseFile( gtv->demoplayback );
        gtv->demoplayback = 0;
    }

    demo_free_playlist( gtv );

    Z_Free( gtv );
}


/*
====================================================================

GTV CONNECTIONS

====================================================================
*/

static void write_stream( gtv_t *gtv, void *data, size_t len ) {
    if( FIFO_Write( &gtv->stream.send, data, len ) != len ) {
        gtv_destroyf( gtv, "Send buffer overflowed" );
    }

    // don't timeout
    gtv->last_sent = svs.realtime;
}

static void write_message( gtv_t *gtv, gtv_clientop_t op ) {
    byte header[3];
    size_t len = msg_write.cursize + 1;

    header[0] = len & 255;
    header[1] = ( len >> 8 ) & 255;
    header[2] = op;
    write_stream( gtv, header, sizeof( header ) );

    write_stream( gtv, msg_write.data, msg_write.cursize );
}

static qboolean gtv_wait_stop( mvd_t *mvd ) {
    int usage;

    // see how many frames are buffered
    if( mvd->num_packets >= mvd->min_packets ) {
        Com_Printf( "[%s] -=- Waiting finished, reading...\n", mvd->name );
        mvd->state = MVD_READING;
        return qtrue;
    }

    // see how much data is buffered
    usage = FIFO_Percent( &mvd->delay );
    if( usage >= mvd_wait_percent->value ) {
        Com_Printf( "[%s] -=- Buffering finished, reading...\n", mvd->name );
        mvd->state = MVD_READING;
        return qtrue;
    }

    return qfalse;
}

// ran out of buffers
static void gtv_wait_start( mvd_t *mvd ) {
    gtv_t *gtv = mvd->gtv;
    int tr = mvd_wait_delay->value * 10;

    // if not connected, kill it
    if( !gtv ) {
        MVD_Destroyf( mvd, "End of MVD stream reached" );
    }

    Com_Printf( "[%s] -=- Buffering data...\n", mvd->name );

    mvd->state = MVD_WAITING;

    if( gtv->state == GTV_READING ) {
        // oops, if this happened in the middle of the game,
        // resume as quickly as possible after there is some
        // data available again
        mvd->min_packets = 50 + 5 * mvd->underflows;
        if( mvd->min_packets > tr ) {
            mvd->min_packets = tr;
        }
        mvd->underflows++;

        // notify spectators
        if( Com_IsDedicated() ) {
            MVD_BroadcastPrintf( mvd, PRINT_HIGH, 0,
                "[MVD] Buffering data, please wait...\n" );
        }

        // send ping to force server to flush
        write_message( gtv, GTC_PING );
    } else {
        // this is a `normal' underflow, reset delay to default
        mvd->min_packets = tr;
        mvd->underflows = 0;
    }

    MVD_CheckActive( mvd );
}

static qboolean gtv_read_frame( mvd_t *mvd ) {
    uint16_t msglen;

    switch( mvd->state ) {
    case MVD_WAITING:
        if( !gtv_wait_stop( mvd ) ) {
            return qfalse;
        }
        break;
    case MVD_READING:
        if( !mvd->num_packets ) {
            gtv_wait_start( mvd );
            return qfalse;
        }
        break;
    default:
        MVD_Destroyf( mvd, "%s: bad mvd->state", __func__ );
    }

    // NOTE: if we got here, delay buffer MUST contain
    // at least one complete, non-empty packet

    // parse msglen
    if( FIFO_Read( &mvd->delay, &msglen, 2 ) != 2 ) {
        MVD_Destroyf( mvd, "%s: partial data", __func__ );
    }

    msglen = LittleShort( msglen );
    if( msglen < 1 || msglen > MAX_MSGLEN ) {
        MVD_Destroyf( mvd, "%s: invalid msglen", __func__ );
    }

    // read this message
    if( !FIFO_ReadMessage( &mvd->delay, msglen ) ) {
        MVD_Destroyf( mvd, "%s: partial data", __func__ );
    }

    // decrement buffered packets counter
    mvd->num_packets--;

    // parse it
    MVD_ParseMessage( mvd );
    return qtrue;
}

static qboolean gtv_forward_cmd( mvd_client_t *client ) {
    mvd_t *mvd = client->mvd;
    gtv_t *gtv = mvd->gtv;
    char *text;
    size_t len;

    if( !gtv || gtv->state < GTV_CONNECTED ) {
	    SV_ClientPrintf( client->cl, PRINT_HIGH,
            "[MVD] Not connected to the game server.\n" );
        return qfalse;
    }
    if( !( gtv->flags & GTF_STRINGCMDS ) ) {
	    SV_ClientPrintf( client->cl, PRINT_HIGH,
            "[MVD] Game server does not allow command forwarding.\n" );
        return qfalse;
    }
    if( FIFO_Usage( &gtv->stream.send ) ) {
	    SV_ClientPrintf( client->cl, PRINT_HIGH,
            "[MVD] Send buffer not empty, please wait.\n" );
        return qfalse;
    }

    text = Cmd_Args();
    len = strlen( text );
    if( len > 150 ) {
        len = 150;
    }

    // send it
    MSG_WriteData( text, len );
    MSG_WriteByte( 0 );
    write_message( gtv, GTC_STRINGCMD );
    SZ_Clear( &msg_write );
    return qtrue;
}

static void send_hello( gtv_t *gtv ) {
    int flags = GTF_STRINGCMDS;

#if USE_ZLIB
    flags |= GTF_DEFLATE;
#endif

    MSG_WriteShort( GTV_PROTOCOL_VERSION );
    MSG_WriteLong( flags );
    MSG_WriteLong( 0 ); // reserved
    MSG_WriteString( gtv->username ? gtv->username : mvd_username->string );
    MSG_WriteString( gtv->password ? gtv->password : mvd_password->string );
    MSG_WriteString( com_version->string );
    write_message( gtv, GTC_HELLO );
    SZ_Clear( &msg_write );

    Com_Printf( "[%s] -=- Sending client hello...\n", gtv->name );

    gtv->state = GTV_PREPARING;
}

static void send_stream_start( gtv_t *gtv ) {
    int maxbuf;

    if( gtv->mvd ) {
        maxbuf = gtv->mvd->min_packets / 2;
    } else {
        maxbuf = mvd_wait_delay->value * 10 / 2;
    }
    if( maxbuf < 10 ) {
        maxbuf = 10;
    }

    // send stream start request
    MSG_WriteShort( maxbuf );
    write_message( gtv, GTC_STREAM_START );
    SZ_Clear( &msg_write );

    Com_Printf( "[%s] -=- Sending stream start request...\n", gtv->name );

    gtv->state = GTV_RESUMING;
}

static void send_stream_stop( gtv_t *gtv ) {
    // send stream stop request
    write_message( gtv, GTC_STREAM_STOP );

    Com_Printf( "[%s] -=- Sending stream stop request...\n", gtv->name );

    gtv->state = GTV_SUSPENDING;
}


#if USE_ZLIB
static voidpf gtv_zalloc OF(( voidpf opaque, uInt items, uInt size )) {
    return MVD_Malloc( items * size );
}

static void gtv_zfree OF(( voidpf opaque, voidpf address )) {
    Z_Free( address );
}
#endif

static void parse_hello( gtv_t *gtv ) {
    int flags;

    if( gtv->state >= GTV_CONNECTED ) {
        gtv_destroyf( gtv, "Duplicated server hello" );
    }

    flags = MSG_ReadLong();

    if( flags & GTF_DEFLATE ) {
#if USE_ZLIB
        if( !gtv->z_str.state ) {
            gtv->z_str.zalloc = gtv_zalloc;
            gtv->z_str.zfree = gtv_zfree;
            if( inflateInit( &gtv->z_str ) != Z_OK ) {
                gtv_destroyf( gtv, "inflateInit() failed: %s",
                    gtv->z_str.msg );
            }
        }
        if( !gtv->z_buf.data ) {
            gtv->z_buf.data = MVD_Malloc( MAX_GTS_MSGLEN );
            gtv->z_buf.size = MAX_GTS_MSGLEN;
        }
        gtv->z_act = qtrue; // remaining data is deflated
#else
        gtv_destroyf( gtv, "Server sending deflated data" );
#endif
    }
    
    Com_Printf( "[%s] -=- Server hello done.\n", gtv->name );

    if( sv.state != ss_broadcast ) {
        MVD_Spawn_f(); // the game is just starting
    }

    gtv->flags = flags;
    gtv->state = GTV_CONNECTED;
}

static void parse_stream_start( gtv_t *gtv ) {
    mvd_t *mvd = gtv->mvd;
    size_t size;

    if( gtv->state != GTV_RESUMING ) {
        gtv_destroyf( gtv, "Unexpected stream start ack in state %u", gtv->state );
    }

    // create the channel
    if( !mvd ) {
        mvd = create_channel( gtv );

        Cvar_ClampInteger( mvd_buffer_size, 2, 10 );

        // allocate delay buffer
        size = mvd_buffer_size->integer * MAX_MSGLEN;
        mvd->delay.data = MVD_Malloc( size );
        mvd->delay.size = size;
        mvd->read_frame = gtv_read_frame;
        mvd->forward_cmd = gtv_forward_cmd;

        gtv->mvd = mvd;
    } else {
        // reset delay to default
        mvd->min_packets = mvd_wait_delay->value * 10;
        mvd->underflows = 0;
    }

    Com_Printf( "[%s] -=- Stream start ack received.\n", gtv->name );

    gtv->state = GTV_READING;
}

static void parse_stream_stop( gtv_t *gtv ) {
    if( gtv->state != GTV_SUSPENDING ) {
        gtv_destroyf( gtv, "Unexpected stream stop ack in state %u", gtv->state );
    }

    Com_Printf( "[%s] -=- Stream stop ack received.\n", gtv->name );

    gtv->state = GTV_CONNECTED;
}

static void parse_stream_data( gtv_t *gtv ) {
    mvd_t *mvd = gtv->mvd;

    if( gtv->state < GTV_WAITING ) {
        gtv_destroyf( gtv, "Unexpected stream data packet" );
    }

    // ignore any pending data while suspending
    if( gtv->state == GTV_SUSPENDING ) {
        msg_read.readcount = msg_read.cursize;
        return;
    }

    // empty data part acts as stream suspend marker
    if( msg_read.readcount == msg_read.cursize ) {
        if( gtv->state == GTV_READING ) {
            Com_Printf( "[%s] -=- Stream suspended by server.\n", gtv->name );
            gtv->state = GTV_WAITING;
        }
        return;
    }

    // non-empty data part act as stream resume marker
    if( gtv->state == GTV_WAITING ) {
        Com_Printf( "[%s] -=- Stream resumed by server.\n", gtv->name );
        gtv->state = GTV_READING;
    }

    if( !mvd->state ) {
        // parse it in place
        MVD_ParseMessage( mvd );
    } else {
        byte *data = msg_read.data + 1;
        size_t len = msg_read.cursize - 1;
        uint16_t msglen;

        // see if this packet fits
        if( FIFO_Write( &mvd->delay, NULL, len + 2 ) != len + 2 ) {
            if( mvd->state == MVD_WAITING ) {
                // if delay buffer overflowed in waiting state,
                // something is seriously wrong, disconnect for safety
                gtv_destroyf( gtv, "Delay buffer overflowed in waiting state" );
            }

            // oops, overflowed
            Com_Printf( "[%s] =!= Delay buffer overflowed!\n", gtv->name );

            if( Com_IsDedicated() ) {
                // notify spectators
                MVD_BroadcastPrintf( mvd, PRINT_HIGH, 0,
                    "[MVD] Delay buffer overflowed!\n" );
            }

            // clear entire delay buffer
            FIFO_Clear( &mvd->delay );
            mvd->state = MVD_WAITING;
            mvd->min_packets = 50;
            mvd->overflows++;

            // send stream stop request
            write_message( gtv, GTC_STREAM_STOP );
            gtv->state = GTV_SUSPENDING;
            return;
        }

        // write it into delay buffer
        msglen = LittleShort( len );
        FIFO_Write( &mvd->delay, &msglen, 2 );
        FIFO_Write( &mvd->delay, data, len );

        // increment buffered packets counter
        mvd->num_packets++;

        msg_read.readcount = msg_read.cursize;
    }
}

static qboolean parse_message( gtv_t *gtv, fifo_t *fifo ) {
    uint32_t magic;
    uint16_t msglen;
    int cmd;

    // check magic
    if( gtv->state < GTV_PREPARING ) {
        if( !FIFO_TryRead( fifo, &magic, 4 ) ) {
            return qfalse;
        }
        if( magic != MVD_MAGIC ) {
            gtv_destroyf( gtv, "Not a MVD/GTV stream" );
        }

        // send client hello
        send_hello( gtv );
    }

    // parse msglen
    if( !gtv->msglen ) {
        if( !FIFO_TryRead( fifo, &msglen, 2 ) ) {
            return qfalse;
        }
        msglen = LittleShort( msglen );
        if( !msglen ) {
            gtv_dropf( gtv, "End of MVD/GTV stream" );
        }
        if( msglen > MAX_MSGLEN ) {
            gtv_destroyf( gtv, "Oversize message" );
        }
        gtv->msglen = msglen;
    }

    // read this message
    if( !FIFO_ReadMessage( fifo, gtv->msglen ) ) {
        return qfalse;
    }

    gtv->msglen = 0;

    cmd = MSG_ReadByte();

    switch( cmd ) {
    case GTS_HELLO:
        parse_hello( gtv );
        break;
    case GTS_PONG:
        break;
    case GTS_STREAM_START:
        parse_stream_start( gtv );
        break;
    case GTS_STREAM_STOP:
        parse_stream_stop( gtv );
        break;
    case GTS_STREAM_DATA:
        parse_stream_data( gtv );
        break;
    case GTS_ERROR:
        gtv_destroyf( gtv, "Server side error occured." );
        break;
    case GTS_BADREQUEST:
        gtv_destroyf( gtv, "Server refused to process our request." );
        break;
    case GTS_NOACCESS:
        gtv_destroyf( gtv, 
            "You don't have permission to access "
            "MVD/GTV stream on this server." );
        break;
    case GTS_DISCONNECT:
        gtv_destroyf( gtv, "Server has been shut down." );
        break;
    case GTS_RECONNECT:
        gtv_dropf( gtv, "Server has been restarted." );
        break;
    default:
        gtv_destroyf( gtv, "Unknown command byte" );
    }

    if( msg_read.readcount > msg_read.cursize ) {
        gtv_destroyf( gtv, "Read past end of message" );
    }
    
    gtv->last_rcvd = svs.realtime; // don't timeout
    return qtrue;
}

#if USE_ZLIB
static int inflate_stream( fifo_t *dst, fifo_t *src, z_streamp z ) {
    byte    *data;
    size_t  avail_in, avail_out;
    int     ret = Z_BUF_ERROR;

    do {
        data = FIFO_Peek( src, &avail_in );
        if( !avail_in ) {
            break;
        }
        z->next_in = data;
        z->avail_in = ( uInt )avail_in;

        data = FIFO_Reserve( dst, &avail_out );
        if( !avail_out ) {
            break;
        }
        z->next_out = data;
        z->avail_out = ( uInt )avail_out;

        ret = inflate( z, Z_SYNC_FLUSH );

        FIFO_Decommit( src, avail_in - z->avail_in );
        FIFO_Commit( dst, avail_out - z->avail_out );
    } while( ret == Z_OK );

    return ret;
}

static void inflate_more( gtv_t *gtv ) {
    int ret = inflate_stream( &gtv->z_buf, &gtv->stream.recv, &gtv->z_str );

    switch( ret ) {
    case Z_BUF_ERROR:
    case Z_OK:
        break;
    case Z_STREAM_END:
        inflateReset( &gtv->z_str );
        gtv->z_act = qfalse;
        break;
    default:
        gtv_destroyf( gtv, "inflate() failed: %s", gtv->z_str.msg );
    }
}
#endif

static neterr_t run_connect( gtv_t *gtv ) {
    neterr_t ret;
    uint32_t magic;

    // run connection
    if( ( ret = NET_RunConnect( &gtv->stream ) ) != NET_OK ) {
        return ret;
    }

    Com_Printf( "[%s] -=- Connected to the game server!\n", gtv->name );

    // allocate buffers
    if( !gtv->data ) {
        gtv->data = MVD_Malloc( MAX_GTS_MSGLEN + MAX_GTC_MSGLEN );
    }
    gtv->stream.recv.data = gtv->data;
    gtv->stream.recv.size = MAX_GTS_MSGLEN;
    gtv->stream.send.data = gtv->data + MAX_GTS_MSGLEN;
    gtv->stream.send.size = MAX_GTC_MSGLEN;

    // don't timeout
    gtv->last_rcvd = svs.realtime;

    // send magic
    magic = MVD_MAGIC;
    write_stream( gtv, &magic, 4 );

    return NET_OK;
}

static neterr_t run_stream( gtv_t *gtv ) {
    neterr_t ret;
    int count;
    size_t usage;

    // run network stream
    if( ( ret = NET_RunStream( &gtv->stream ) ) != NET_OK ) {
        return ret;
    }

    count = 0;
    usage = FIFO_Usage( &gtv->stream.recv );

#if USE_ZLIB
    if( gtv->z_act ) {
        while( 1 ) {
            // decompress more data
            if( gtv->z_act ) {
                inflate_more( gtv );
            }
            if( !parse_message( gtv, &gtv->z_buf ) ) {
                break;
            }
            count++;
        }
    } else
#endif
        while( parse_message( gtv, &gtv->stream.recv ) ) {
            count++;
        }

    if( mvd_shownet->integer == -1 ) {
        size_t total = usage - FIFO_Usage( &gtv->stream.recv );

        Com_Printf( "[%s] %"PRIz" bytes, %d msgs\n",
            gtv->name, total, count );
    }

    return NET_OK;
}

static void check_timeouts( gtv_t *gtv ) {
    unsigned timeout = mvd_timeout->value * 1000;

    // drop if no data has been received for too long
    if( svs.realtime - gtv->last_rcvd > timeout ) {
        gtv_dropf( gtv, "Server connection timed out." );
    }

    if( gtv->state < GTV_CONNECTED ) {
        return;
    }

    // stop/start stream depending on global state
    if( mvd_active ) {
        if( gtv->state == GTV_CONNECTED ) {
            send_stream_start( gtv );
        }
    } else if( gtv->state == GTV_WAITING || gtv->state == GTV_READING ) {
        send_stream_stop( gtv );
    }

    // ping if no data has been sent for too long
    if( svs.realtime - gtv->last_sent > GTV_PING_INTERVAL ) {
        write_message( gtv, GTC_PING );
    }
}

static qboolean check_reconnect( gtv_t *gtv ) {
    netadr_t adr;

    if( svs.realtime - gtv->retry_time < gtv->retry_backoff ) {
        return qfalse;
    }

    Com_Printf( "[%s] -=- Attempting to reconnect to %s...\n",
        gtv->name, gtv->address );

    gtv->state = GTV_CONNECTING;

    // don't timeout
    gtv->last_sent = gtv->last_rcvd = svs.realtime;

    if( !NET_StringToAdr( gtv->address, &adr, PORT_SERVER ) ) {
        gtv_dropf( gtv, "Unable to lookup %s\n", gtv->address );
    }

    if( NET_Connect( &adr, &gtv->stream ) == NET_ERROR ) {
        gtv_dropf( gtv, "%s to %s\n", NET_ErrorString(),
            NET_AdrToString( &adr ) );
    }

    return qtrue;
}

static void gtv_run( gtv_t *gtv ) {
    neterr_t ret = NET_AGAIN;

    // check if it is time to reconnect
    if( !gtv->state ) {
        if( !check_reconnect( gtv ) ) {
            return;
        }
    }

    // run network stream
    switch( gtv->stream.state ) {
    case NS_CONNECTING:
        ret = run_connect( gtv );
        if( ret == NET_AGAIN ) {
            return;
        }
        if( ret == NET_OK ) {
    case NS_CONNECTED:
            ret = run_stream( gtv );
        }
        break;
    default:
        return;
    }

    switch( ret ) {
    case NET_AGAIN:
    case NET_OK:
        check_timeouts( gtv );
        break;
    case NET_ERROR:
        gtv_dropf( gtv, "%s to %s", NET_ErrorString(),
            NET_AdrToString( &gtv->stream.address ) );
        break;
    case NET_CLOSED:
        gtv_dropf( gtv, "Server has closed connection." );
        break;
    }
}

static void gtv_destroy( gtv_t *gtv ) {
    mvd_t *mvd = gtv->mvd;

    // any associated MVD channel is orphaned
    if( mvd ) {
        mvd->gtv = NULL;
        if( !mvd->state ) {
            // free it here, since it is not yet
            // added to global channel list
            MVD_Free( mvd );
        } else if( Com_IsDedicated() ) {
            // notify spectators
            MVD_BroadcastPrintf( mvd, PRINT_HIGH, 0,
                "[MVD] Disconnected from the game server!\n" );
        }
    }

    // make sure network connection is closed
    NET_Close( &gtv->stream );

    // unlink from the list of connections
    List_Remove( &gtv->entry );

    // free all memory buffers
    Z_Free( gtv->username );
    Z_Free( gtv->password );
#if USE_ZLIB
    inflateEnd( &gtv->z_str );
    Z_Free( gtv->z_buf.data );
#endif
    Z_Free( gtv->data );
    Z_Free( gtv );
}

static void gtv_drop( gtv_t *gtv ) {
    if( gtv->stream.state < NS_CONNECTED ) {
        gtv->retry_backoff += 15*1000;
    } else {
        // notify spectators
        if( Com_IsDedicated() && gtv->mvd ) {
            MVD_BroadcastPrintf( gtv->mvd, PRINT_HIGH, 0,
                "[MVD] Lost connection to the game server!\n" );
        }

        if( gtv->state >= GTV_CONNECTED ) {
            gtv->retry_backoff = GTV_DEFAULT_BACKOFF;
        } else {
            gtv->retry_backoff += 30*1000;
        }
    }

    if( gtv->retry_backoff > GTV_MAXIMUM_BACKOFF ) {
        gtv->retry_backoff = GTV_MAXIMUM_BACKOFF;
    }
    Com_Printf( "[%s] -=- Reconnecting in %d seconds.\n",
        gtv->name, gtv->retry_backoff / 1000 );

    NET_Close( &gtv->stream );
#if USE_ZLIB
    inflateReset( &gtv->z_str );
    FIFO_Clear( &gtv->z_buf );
    gtv->z_act = qfalse;
#endif
    gtv->msglen = 0;
    gtv->state = GTV_DISCONNECTED;
    gtv->retry_time = svs.realtime;
}


/*
====================================================================

OPERATOR COMMANDS

====================================================================
*/

void MVD_Spawn_f( void ) {
    SV_InitGame( qtrue );

    Cvar_SetInteger( sv_running, ss_broadcast, CVAR_SET_DIRECT );
    Cvar_Set( "sv_paused", "0" );
    Cvar_Set( "timedemo", "0" );
    SV_InfoSet( "port", net_port->string );

    SV_SetConsoleTitle();

    // generate spawncount for Waiting Room
    sv.spawncount = ( rand() | ( rand() << 16 ) ) ^ Sys_Milliseconds();
    sv.spawncount &= 0x7FFFFFFF;

    sv.state = ss_broadcast;
}

static void MVD_ListChannels_f( void ) {
    mvd_t *mvd;

    if( LIST_EMPTY( &mvd_channel_list ) ) {
        Com_Printf( "No MVD channels.\n" );
        return;
    }

    Com_Printf(
        "id name         map      spc plr stat buf pckt address       \n"
        "-- ------------ -------- --- --- ---- --- ---- --------------\n" );

    LIST_FOR_EACH( mvd_t, mvd, &mvd_channel_list, entry ) {
        Com_Printf( "%2d %-12.12s %-8.8s %3d %3d %-4.4s %3d %4u %s\n",
            mvd->id, mvd->name, mvd->mapname,
            List_Count( &mvd->clients ), mvd->numplayers,
            mvd_states[mvd->state],
            FIFO_Percent( &mvd->delay ), mvd->num_packets,
            mvd->gtv ? mvd->gtv->address : "<disconnected>" );
    }
}

static void MVD_ListServers_f( void ) {
    gtv_t *gtv;
    unsigned ratio;

    if( LIST_EMPTY( &mvd_gtv_list ) ) {
        Com_Printf( "No GTV connections.\n" );
        return;
    }

    Com_Printf(
        "id name         state        ratio lastmsg address       \n"
        "-- ------------ ------------ ----- ------- --------------\n" );

    LIST_FOR_EACH( gtv_t, gtv, &mvd_gtv_list, entry ) {
        ratio = 100;
#if USE_ZLIB
        if( gtv->z_act && gtv->z_str.total_out ) {
            ratio = 100 * ( ( double )gtv->z_str.total_in /
                gtv->z_str.total_out );
        }
#endif
        Com_Printf( "%2d %-12.12s %-12.12s %4u%% %7u %s\n",
            gtv->id, gtv->name, gtv_states[gtv->state],
            ratio, svs.realtime - gtv->last_rcvd,
            NET_AdrToString( &gtv->stream.address ) );
    }
}

void MVD_StreamedStop_f( void ) {
    mvd_t *mvd;
    uint16_t msglen;

    mvd = MVD_SetChannel( 1 );
    if( !mvd ) {
        Com_Printf( "Usage: %s [chanid]\n", Cmd_Argv( 0 ) );
        return;
    }

    if( !mvd->demorecording ) {
        Com_Printf( "[%s] Not recording a demo.\n", mvd->name );
        return;
    }

    msglen = 0;
    FS_Write( &msglen, 2, mvd->demorecording );

    FS_FCloseFile( mvd->demorecording );
    mvd->demorecording = 0;

    Com_Printf( "[%s] Stopped recording.\n", mvd->name );
}

static void MVD_EmitGamestate( mvd_t *mvd ) {
    char        *string;
    int         i;
    edict_t     *ent;
    player_state_t *ps;
    size_t      length;
    int         flags, portalbytes;
    byte        portalbits[MAX_MAP_AREAS/8];

    // send the serverdata
    MSG_WriteByte( mvd_serverdata );
    MSG_WriteLong( PROTOCOL_VERSION_MVD );
    MSG_WriteShort( PROTOCOL_VERSION_MVD_CURRENT );
    MSG_WriteLong( mvd->servercount );
    MSG_WriteString( mvd->gamedir );
    MSG_WriteShort( mvd->clientNum );

    // send configstrings
    for( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
        string = mvd->configstrings[i];
        if( !string[0] ) {
            continue;
        }
        length = strlen( string );
        if( length > MAX_QPATH ) {
            length = MAX_QPATH;
        }

        MSG_WriteShort( i );
        MSG_WriteData( string, length );
        MSG_WriteByte( 0 );
    }
    MSG_WriteShort( MAX_CONFIGSTRINGS );

    // send baseline frame
    portalbytes = CM_WritePortalBits( &sv.cm, portalbits );
    MSG_WriteByte( portalbytes );
    MSG_WriteData( portalbits, portalbytes );
    
    // send base player states
    for( i = 0; i < mvd->maxclients; i++ ) {
        ps = &mvd->players[i].ps;
        flags = 0;
        if( !PPS_INUSE( ps ) ) {
            flags |= MSG_PS_REMOVE;
        }
        MSG_WriteDeltaPlayerstate_Packet( NULL, ps, i, flags );
    }
    MSG_WriteByte( CLIENTNUM_NONE );

    // send base entity states
    for( i = 1; i < mvd->pool.num_edicts; i++ ) {
        ent = &mvd->edicts[i];
        flags = 0;
        if( i <= mvd->maxclients ) {
            ps = &mvd->players[ i - 1 ].ps;
            if( PPS_INUSE( ps ) && ps->pmove.pm_type == PM_NORMAL ) {
                flags |= MSG_ES_FIRSTPERSON;
            }
        }
        if( !ent->inuse ) {
            flags |= MSG_ES_REMOVE;
        }
        MSG_WriteDeltaEntity( NULL, &ent->s, flags );
    }
    MSG_WriteShort( 0 );

    // TODO: write private layouts/configstrings
}

extern const cmd_option_t o_mvdrecord[];

void MVD_StreamedRecord_f( void ) {
    char buffer[MAX_OSPATH];
    fileHandle_t f;
    mvd_t *mvd;
    uint32_t magic;
    uint16_t msglen;
    qboolean gzip = qfalse;
    int c;
    size_t len;

    while( ( c = Cmd_ParseOptions( o_mvdrecord ) ) != -1 ) {
        switch( c ) {
        case 'h':
            Cmd_PrintUsage( o_mvdrecord, "[/]<filename> [chanid]" );
            Com_Printf( "Begin MVD recording on the specified channel.\n" );
            Cmd_PrintHelp( o_mvdrecord );
            return;
        case 'z':
            gzip = qtrue;
            break;
        default:
            return;
        }
    }

    if( !cmd_optarg[0] ) {
        Com_Printf( "Missing filename argument.\n" );
        Cmd_PrintHint();
        return;
    }
    
    if( ( mvd = MVD_SetChannel( cmd_optind + 1 ) ) == NULL ) {
        Cmd_PrintHint();
        return;
    }

    if( mvd->demorecording ) {
        Com_Printf( "[%s] Already recording.\n", mvd->name );
        return;
    }

    //
    // open the demo file
    //
    len = Q_concat( buffer, sizeof( buffer ), "demos/", cmd_optarg,
        gzip ? ".mvd2.gz" : ".mvd2", NULL );
    if( len >= sizeof( buffer ) ) {
        Com_EPrintf( "Oversize filename specified.\n" );
        return;
    }

    FS_FOpenFile( buffer, &f, FS_MODE_WRITE );
    if( !f ) {
        Com_EPrintf( "Couldn't open %s for writing\n", buffer );
        return;
    }
    
    Com_Printf( "[%s] Recording into %s\n", mvd->name, buffer );

    if( gzip ) {
        FS_FilterFile( f );
    }

    mvd->demorecording = f;

    MVD_EmitGamestate( mvd );

    // write magic
    magic = MVD_MAGIC;
    FS_Write( &magic, 4, f );

    // write gamestate
    msglen = LittleShort( msg_write.cursize );
    FS_Write( &msglen, 2, f );
    FS_Write( msg_write.data, msg_write.cursize, f );

    SZ_Clear( &msg_write );
}

static const cmd_option_t o_mvdconnect[] = {
    { "h", "help", "display this message" },
    { "n:string", "name", "specify channel name as <string>" },
    { "u:string", "user", "specify username as <string>" },
    { "p:string", "pass", "specify password as <string>" },
    { NULL }
};

static void MVD_Connect_c( genctx_t *ctx, int argnum ) {
    Cmd_Option_c( o_mvdconnect, Com_Address_g, ctx, argnum );
}

/*
==============
MVD_Connect_f
==============
*/
static void MVD_Connect_f( void ) {
    netadr_t adr;
    netstream_t stream;
    char *name = NULL, *username = NULL, *password = NULL;
    gtv_t *gtv;
    int c;

    while( ( c = Cmd_ParseOptions( o_mvdconnect ) ) != -1 ) {
        switch( c ) {
        case 'h':
            Cmd_PrintUsage( o_mvdconnect, "<address[:port]>" );
            Com_Printf( "Connect to the specified MVD/GTV server.\n" );
            Cmd_PrintHelp( o_mvdconnect );
            return;
        case 'n':
            name = cmd_optarg;
            break;
        case 'u':
            username = cmd_optarg;
            break;
        case 'p':
            password = cmd_optarg;
            break;
        default:
            return;
        }
    }

    if( !cmd_optarg[0] ) {
        Com_Printf( "Missing address argument.\n" );
        Cmd_PrintHint();
        return;
    }

    // resolve hostname
    if( !NET_StringToAdr( cmd_optarg, &adr, PORT_SERVER ) ) {
        Com_Printf( "Bad server address: %s\n", cmd_optarg );
        return;
    }

    // don't allow multiple connections
    LIST_FOR_EACH( gtv_t, gtv, &mvd_gtv_list, entry ) {
        if( NET_IsEqualAdr( &adr, &gtv->stream.address ) ) {
            Com_Printf( "[%s] =!= Already connected to %s\n",
                gtv->name, NET_AdrToString( &adr ) );
            return;
        }
    }

    // create new socket and start connecting
    if( NET_Connect( &adr, &stream ) == NET_ERROR ) {
        Com_EPrintf( "%s to %s\n", NET_ErrorString(),
            NET_AdrToString( &adr ) );
        return;
    }

    // create new connection
    gtv = MVD_Mallocz( sizeof( *gtv ) );
    gtv->id = mvd_chanid++;
    gtv->state = GTV_CONNECTING;
    gtv->stream = stream;
    gtv->last_sent = gtv->last_rcvd = svs.realtime;
    gtv->run = gtv_run;
    gtv->drop = gtv_drop;
    gtv->destroy = gtv_destroy;
    gtv->username = MVD_CopyString( username );
    gtv->password = MVD_CopyString( password );
    List_Append( &mvd_gtv_list, &gtv->entry );

    // set channel name
    if( name ) {
        Q_strlcpy( gtv->name, name, sizeof( gtv->name ) );
    } else {
        Q_snprintf( gtv->name, sizeof( gtv->name ), "net%d", gtv->id );
    }

    Q_strlcpy( gtv->address, cmd_optarg, sizeof( gtv->address ) );

    Com_Printf( "[%s] -=- Connecting to %s...\n",
        gtv->name, NET_AdrToString( &adr ) );
}

static void MVD_Disconnect_f( void ) {
    gtv_t *gtv;

    gtv = gtv_set_conn( 1 );
    if( !gtv ) {
        return;
    }

    Com_Printf( "[%s] =X= Connection destroyed.\n", gtv->name );
    gtv->destroy( gtv );
}

static void MVD_Kill_f( void ) {
    mvd_t *mvd;

    mvd = MVD_SetChannel( 1 );
    if( !mvd ) {
        return;
    }

    Com_Printf( "[%s] =X= Channel was killed.\n", mvd->name );
    MVD_Destroy( mvd );
}

static void MVD_Pause_f( void ) {
    mvd_t *mvd;

    mvd = MVD_SetChannel( 1 );
    if( !mvd ) {
        return;
    }

    if( !mvd->gtv || !mvd->gtv->demoplayback ) {
        Com_Printf( "[%s] Only demo channels can be paused.\n", mvd->name );
        return;
    }

    switch( mvd->state ) {
    case MVD_WAITING:
        //Com_Printf( "[%s] Channel was resumed.\n", mvd->name );
        mvd->state = MVD_READING;
        break;
    case MVD_READING:
        //Com_Printf( "[%s] Channel was paused.\n", mvd->name );
        mvd->state = MVD_WAITING;
        break;
    default:
        break;
    }
}

static void MVD_Control_f( void ) {
    static const cmd_option_t options[] = {
        { "h", "help", "display this message" },
        { "l:number", "loop", "replay <number> of times (0 means forever)" },
        { "n:string", "name", "specify channel name as <string>" },
        { NULL }
    };
    mvd_t *mvd;
    char *name = NULL;
    int loop = -1;
    int todo = 0;
    int c;

    while( ( c = Cmd_ParseOptions( options ) ) != -1 ) {
        switch( c ) {
        case 'h':
            Cmd_PrintUsage( options, "[chanid]" );
            Com_Printf( "Change attributes of existing MVD channel.\n" );
            Cmd_PrintHelp( options );
            return;
        case 'l':
            loop = atoi( cmd_optarg );
            if( loop < 0 ) {
                Com_Printf( "Invalid value for %s option.\n", cmd_optopt );
                Cmd_PrintHint();
                return;
            }
            todo |= 1;
            break;
        case 'n':
            name = cmd_optarg;
            todo |= 2;
            break;
        default:
            return;
        }
    }

    if( !todo ) {
        Com_Printf( "At least one option needed.\n" );
        Cmd_PrintHint();
        return;
    }

    mvd = MVD_SetChannel( cmd_optind );
    if( !mvd ) {
        Cmd_PrintHint();
        return;
    }

    if( name ) {
        Com_Printf( "[%s] Channel renamed to %s.\n", mvd->name, name );
        Q_strlcpy( mvd->name, name, sizeof( mvd->name ) );
    }
    if( loop != -1 ) {
        //Com_Printf( "[%s] Loop count changed to %d.\n", mvd->name, loop );
        //mvd->demoloop = loop;
    }
}

static const cmd_option_t o_mvdplay[] = {
    { "h", "help", "display this message" },
    { "l:number", "loop", "replay <number> of times (0 means forever)" },
    { "n:string", "name", "specify channel name as <string>" },
    //{ "i:chan_id", "insert", "insert new entries before <chan_id> playlist" },
    //{ "a:chan_id", "append", "append new entries after <chan_id> playlist" },
    { "r:chan_id", "replace", "replace <chan_id> playlist with new entries" },
    { NULL }
};

void MVD_File_g( genctx_t *ctx ) {
    FS_File_g( "demos", "*.mvd2;*.mvd2.gz", FS_SEARCH_SAVEPATH | FS_SEARCH_BYFILTER, ctx );
}

static void MVD_Play_c( genctx_t *ctx, int argnum ) {
    Cmd_Option_c( o_mvdplay, MVD_File_g, ctx, argnum );
}

static void MVD_Play_f( void ) {
    char *name = NULL, *s;
    char buffer[MAX_OSPATH];
    int loop = -1, chan_id = -1;
    size_t len;
    gtv_t *gtv = NULL;
    int c, argc;
    string_entry_t *entry, *head;
    int i;

    while( ( c = Cmd_ParseOptions( o_mvdplay ) ) != -1 ) {
        switch( c ) {
        case 'h':
            Cmd_PrintUsage( o_mvdplay, "[/]<filename> [...]" );
            Com_Printf( "Create new MVD channel and begin demo playback.\n" );
            Cmd_PrintHelp( o_mvdplay );
            Com_Printf( "Final path is formatted as demos/<filename>.mvd2.\n"
                "Prepend slash to specify raw path.\n" );
            return;
        case 'l':
            loop = atoi( cmd_optarg );
            if( loop < 0 ) {
                Com_Printf( "Invalid value for %s option.\n", cmd_optopt );
                Cmd_PrintHint();
                return;
            }
            break;
        case 'n':
            name = cmd_optarg;
            break;
        case 'r':
            chan_id = cmd_optind - 1;
            break;
        default:
            return;
        }
    }

    argc = Cmd_Argc();
    if( cmd_optind == argc ) {
        Com_Printf( "Missing filename argument.\n" );
        Cmd_PrintHint();
        return;
    }

    if( chan_id != -1 ) {
        mvd_t *mvd = MVD_SetChannel( chan_id );
        if( mvd ) {
            gtv = mvd->gtv;
        }
    }

    // build the playlist
    head = NULL;
    for( i = argc - 1; i >= cmd_optind; i-- ) {
        s = Cmd_Argv( i );
        if( *s == '/' ) {
            Q_strlcpy( buffer, s + 1, sizeof( buffer ) );
        } else {
            Q_concat( buffer, sizeof( buffer ), "demos/", s, NULL );
            if( FS_LoadFile( buffer, NULL ) == INVALID_LENGTH ) {
                COM_DefaultExtension( buffer, ".mvd2", sizeof( buffer ) );
            }
        }
        if( FS_LoadFile( buffer, NULL ) == INVALID_LENGTH ) {
            Com_Printf( "Ignoring non-existent entry: %s\n", buffer );
            continue;
        }

        len = strlen( buffer );
        entry = MVD_Malloc( sizeof( *entry ) + len );
        memcpy( entry->string, buffer, len + 1 );
        entry->next = head;
        head = entry;
    }

    if( !head ) {
        return;
    }

    if( gtv ) {
        // free existing playlist
        demo_free_playlist( gtv );
    } else {
        // create new connection
        gtv = MVD_Mallocz( sizeof( *gtv ) );
        gtv->id = mvd_chanid++;
        gtv->state = GTV_READING;
        gtv->drop = demo_destroy;
        gtv->destroy = demo_destroy;
        gtv->demoloop = 1;
        Q_snprintf( gtv->name, sizeof( gtv->name ), "dem%d", gtv->id );
    }

    // set channel name
    if( name ) {
        Q_strlcpy( gtv->name, name, sizeof( gtv->name ) );
    }

    // set loop parameter
    if( loop != -1 ) {
        gtv->demoloop = loop;
    }

    // set new playlist
    gtv->demohead = head;
    gtv->demoloop = loop;

    demo_play_next( gtv, head );
}


void MVD_Shutdown( void ) {
    gtv_t *gtv, *gtv_next;
    mvd_t *mvd, *mvd_next;

    // kill all connections
    LIST_FOR_EACH_SAFE( gtv_t, gtv, gtv_next, &mvd_gtv_list, entry ) {
        gtv->destroy( gtv );
    }

    // kill all channels
    LIST_FOR_EACH_SAFE( mvd_t, mvd, mvd_next, &mvd_channel_list, entry ) {
        MVD_Free( mvd );
    }

    List_Init( &mvd_gtv_list );
    List_Init( &mvd_channel_list );
    List_Init( &mvd_active_list );

    Z_Free( mvd_clients );
    mvd_clients = NULL;

    mvd_chanid = 0;

    mvd_active = qfalse;

    Z_LeakTest( TAG_MVD );
}

static const cmdreg_t c_mvd[] = {
    { "mvdplay", MVD_Play_f, MVD_Play_c },
    { "mvdconnect", MVD_Connect_f, MVD_Connect_c },
    { "mvdisconnect", MVD_Disconnect_f },
    { "mvdkill", MVD_Kill_f },
    { "mvdspawn", MVD_Spawn_f },
    { "mvdchannels", MVD_ListChannels_f },
    { "mvdservers", MVD_ListServers_f },
    { "mvdcontrol", MVD_Control_f },
    { "mvdpause", MVD_Pause_f },

    { NULL }
};


/*
==============
MVD_Register
==============
*/
void MVD_Register( void ) {
    mvd_shownet = Cvar_Get( "mvd_shownet", "0", 0 );
    mvd_timeout = Cvar_Get( "mvd_timeout", "90", 0 );
    mvd_suspend_time = Cvar_Get( "mvd_suspend_time", "5", 0 );
    mvd_wait_delay = Cvar_Get( "mvd_wait_delay", "20", 0 );
    mvd_wait_percent = Cvar_Get( "mvd_wait_percent", "35", 0 );
    mvd_buffer_size = Cvar_Get( "mvd_buffer_size", "3", 0 );
    mvd_username = Cvar_Get( "mvd_username", "unnamed", 0 );
    mvd_password = Cvar_Get( "mvd_password", "", CVAR_PRIVATE );

    Cmd_Register( c_mvd );
}

