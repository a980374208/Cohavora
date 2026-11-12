# Wayland video capture

The Linux desktop backend uses the desktop portal ScreenCast protocol and a
private PipeWire input stream. It implements video capture only. System audio,
RemoteDesktop input, annotation coordinates and persistent permission restore
are outside this backend.

## Ownership and lifecycle

`WaylandScreencast` is owned by the existing screen-share session. `Start` starts
one worker and returns; its private GLib context owns every portal operation and
frame delivery. The worker reports `AwaitingPermission` before any other status.
It pins the portal's unique bus owner, subscribes before sending requests and
matches the returned request path, including a response arriving before the
method reply. The connection and method setup have separate finite budgets;
waiting for the user in the system picker does not consume the first-frame
budget. `Ready` follows a valid, streaming PipeWire format and precedes frames.

The private bus connection uses the normal desktop session address. Portal
request/session handles, node IDs, remote descriptors and source titles stay
inside the backend. Status and diagnostic output contain fixed values only.
Response `1` means user cancellation, while other nonzero responses mean
failure. Only a typed D-Bus access/permission-denied error reports `Denied`.

`Stop` is called by the session owner, never a capture callback. It revokes new
delivery, cancels asynchronous work, wakes and joins the worker. Cleanup stops
and joins PipeWire callbacks before destroying stream objects, closes outstanding
portal requests and the session, drains asynchronous completions, and closes the
private bus connection. That last step also revokes requests whose late method
reply was cancelled. The caller's portal FD and PipeWire's duplicated FD have
separate ownership.

## Buffer and media path

The first implementation offers BGRA/BGRx/RGBA/RGBx with MemPtr/MemFd storage.
It does not offer DMA-BUF modifiers and performs no graphics readback. PipeWire
maps supported CPU buffers; the callback validates bounds, stride, crop,
corruption flags and transformation metadata. Invalid or unsupported buffers are
dropped. Source dimensions are bounded to 16,384 per side and 8,192 × 4,320
pixels in total.

There is one latest-frame mailbox. A dequeued PipeWire buffer is copied into
application-owned memory and returned before conversion or publication. The
portal worker converts the newest frame to I420, undoes the buffer's SPA
transform (clockwise rotation followed by horizontal reflection) and applies the
existing `ResolveScreenShareProfile` scaling policy, then uses the existing
screen-share source and preview route. Quality changes use a mailbox; the
applied profile follows the delivered frame. No PipeWire callback calls Qt or
waits for the UI, renderer or network.

The source list contains two capture intents, screen and window. The system
picker supplies the authorized source; these entries are not fabricated monitor
identities. No Windows-style physical display binding is claimed.

## OBS behavior reference

The read-only OBS reference at commit
`fe1ed7955f300efd17fc59f65938388e43d8bb06` was used to compare ScreenCast request
ordering, FD ownership, PipeWire buffer/crop/transform handling and teardown.
This backend is an independent implementation; it does not copy OBS's GPL
implementation or its graphics pipeline.

Build and isolated fake-portal tests establish only their stated scope. Actual
picker consent, screen/window frames, remote reception and desktop resource
release require a real Wayland session and two-participant runtime acceptance.
