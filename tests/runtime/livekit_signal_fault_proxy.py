"""Loopback-only TCP proxy for an opt-in LiveKit signaling fault probe."""

import argparse
import asyncio


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--control-port", type=int, required=True)
    parser.add_argument("--target-host", required=True)
    parser.add_argument("--target-port", type=int, required=True)
    args = parser.parse_args()
    loop = asyncio.get_running_loop()
    active: set[tuple[asyncio.StreamWriter, asyncio.StreamWriter]] = set()
    blocked_until = 0.0

    async def relay(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while chunk := await reader.read(65536):
                writer.write(chunk)
                await writer.drain()
        except (ConnectionError, OSError):
            pass

    async def client(reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter) -> None:
        if loop.time() < blocked_until:
            print("[PROXY_BLOCKED]", flush=True)
            writer.close()
            await writer.wait_closed()
            return
        print("[PROXY_ACCEPT]", flush=True)
        try:
            upstream_reader, upstream_writer = await asyncio.open_connection(
                args.target_host, args.target_port)
        except (ConnectionError, OSError):
            print("[PROXY_UPSTREAM_FAILED]", flush=True)
            writer.close()
            await writer.wait_closed()
            return
        print("[PROXY_UPSTREAM_CONNECTED]", flush=True)
        pair = (writer, upstream_writer)
        active.add(pair)
        tasks = [asyncio.create_task(relay(reader, upstream_writer)),
                 asyncio.create_task(relay(upstream_reader, writer))]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            for channel in pair:
                channel.close()
            await asyncio.gather(*tasks, return_exceptions=True)
            for channel in pair:
                try:
                    await channel.wait_closed()
                except (ConnectionError, OSError):
                    pass
            active.discard(pair)
            print("[PROXY_CLOSED]", flush=True)

    async def control(reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        nonlocal blocked_until
        try:
            command = (await reader.readline()).decode("ascii", "strict").strip()
            parts = command.split()
            if len(parts) != 2 or parts[0] != "fault" or not parts[1].isdigit():
                writer.write(b"INVALID\n")
            else:
                seconds = int(parts[1])
                if not 1 <= seconds <= 10:
                    writer.write(b"INVALID\n")
                else:
                    blocked_until = loop.time() + seconds
                    print(f"[PROXY_FAULT] active={len(active)} seconds={seconds}",
                          flush=True)
                    for pair in tuple(active):
                        for channel in pair:
                            channel.close()
                    writer.write(b"OK\n")
            await writer.drain()
        except (UnicodeError, ConnectionError, OSError):
            pass
        finally:
            writer.close()
            await writer.wait_closed()

    signal_server = await asyncio.start_server(client, "127.0.0.1", args.listen_port)
    control_server = await asyncio.start_server(control, "127.0.0.1", args.control_port)
    async with signal_server, control_server:
        print("[PROXY_READY]", flush=True)
        await asyncio.gather(signal_server.serve_forever(),
                             control_server.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
