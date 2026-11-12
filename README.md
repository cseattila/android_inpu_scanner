# android_inpu_scanner

This program reads Linux input events on the Android device and sends them to
TCP clients on port `6123`.

It broadcasts input event records as newline-delimited text. It also sends a
`PING` line to each connected client every five seconds. Clients can use these
lines to detect when the scanner or its network connection becomes unavailable;
`PING` is not an input event.

Build with the Android NDK configured in `Makefile`:

```sh
make
```

The default input device is `/dev/input/event2`. An alternate device path can
be supplied as the first command-line argument.
