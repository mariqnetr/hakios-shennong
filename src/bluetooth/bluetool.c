/*
 * BlueTool for the Xiaomi 14 Pro: bluetoothd's com.apple.BlueTool. Apple's sends a Broadcom or Apple chip its firmware
 * and says how that went; here the kernel has given the WCN7850 its own (bsd/dev/shennong_bt.c) before bluetoothd
 * could ask, so this only says how it went: script "boot", "init", "deepsleep" and every command -- done.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern xpc_connection_t xpc_dictionary_get_remote_connection(xpc_object_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern const char *xpc_dictionary_get_string(xpc_object_t, const char *);
extern const void *xpc_get_type(xpc_object_t);
extern void xpc_release(xpc_object_t);
extern char *xpc_copy_description(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection;

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(), 1 /* LISTENER */);

	setbuf(stdout, NULL);
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) == &_xpc_type_dictionary) {
				const char *script = xpc_dictionary_get_string(message, "script");
				const char *command = xpc_dictionary_get_string(message, "command");
				xpc_object_t reply = xpc_dictionary_create_reply(message);

				printf("bluetool: script %s command %s\n", script ? script : "-", command ? command : "-");
				if (reply != NULL) {
					xpc_dictionary_set_int64(reply, "result", 0);
					xpc_dictionary_set_string(reply, "firmware", "BTFW.HAMILTON");
					xpc_dictionary_set_int64(reply, "manufacturer", 29);
					xpc_dictionary_set_string(reply, "output", "");
					xpc_connection_send_message(xpc_dictionary_get_remote_connection(message), reply);
					xpc_release(reply);
				}
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	dispatch_main();
}
