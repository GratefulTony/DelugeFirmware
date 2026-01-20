#! /usr/bin/env python3
"""
Memory Profile Collection Tool

End-to-end workflow for collecting memory profile data from Deluge:
1. Build firmware with ENABLE_MEMORY_PROFILE=ON
2. Upload via sysex
3. Collect memory stats via MIDI sysex
4. Write to CSV

Usage:
  dbt memory-profile              # Full workflow (build, upload, collect)
  dbt memory-profile --build      # Build only
  dbt memory-profile --upload     # Upload only
  dbt memory-profile --collect    # Collect data only
"""

import argparse
import csv
import time
import sys
import util


def argparser():
    parser = argparse.ArgumentParser(
        prog="memory-profile",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Collect memory profile data from Deluge",
        epilog="""\nWorkflow:
  1. dbt memory-profile --build    # Build with memory profiling enabled
  2. dbt memory-profile --upload   # Upload firmware via sysex
  3. dbt memory-profile --collect  # Collect data to CSV
  4. dbt memory-profile            # All of the above""",
        exit_on_error=False,
    )
    parser.group = "Development"
    parser.add_argument("--build", action="store_true", help="Build firmware only")
    parser.add_argument("--upload", action="store_true", help="Upload firmware only")
    parser.add_argument("--collect", action="store_true", help="Collect data only")
    parser.add_argument(
        "-o",
        "--output",
        default="memory_profile.csv",
        help="Output CSV file (default: memory_profile.csv)",
    )
    parser.add_argument(
        "-d",
        "--duration",
        type=int,
        default=None,
        help="Collection duration in seconds (default: run until Ctrl+C or Enter)",
    )
    parser.add_argument(
        "-i",
        "--interval",
        type=int,
        default=5,
        help="Sampling interval in seconds for periodic mode (default: 5)",
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=None,
        help="MIDI port number (auto-detect if not specified)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Show all debug messages (not just memory stats)",
    )
    return parser


def build_firmware():
    """Build firmware with ENABLE_MEMORY_PROFILE=ON"""
    util.note("")
    util.note("=" * 60)
    util.note("FIRMWARE BUILD")
    util.note("=" * 60)
    util.note("")
    util.note("Building firmware with memory profiling enabled.")
    util.note("Using 'relwithdebinfo' target (required for debug output).")
    util.note("")

    # Configure with memory profile flag and sysex loading
    util.note("[1/2] Configuring CMake...")
    util.note("      Flags: ENABLE_MEMORY_PROFILE=ON, ENABLE_SYSEX_LOAD=YES")
    util.note("      This may take 1-2 minutes...")
    util.note("")
    ret = util.run(
        ["./dbt", "configure", "-DENABLE_MEMORY_PROFILE=ON", "-DENABLE_SYSEX_LOAD=YES"]
    )
    if ret != 0:
        raise RuntimeError("Configure failed")

    util.note("")
    util.note("[2/2] Building firmware...")
    util.note("      This typically takes 5-10 minutes for a full build.")
    util.note("      (Incremental builds are faster)")
    util.note("")

    # Build relwithdebinfo - required for ENABLE_TEXT_OUTPUT which Debug::println needs
    ret = util.run(["./dbt", "build", "relwithdebinfo"])
    if ret != 0:
        raise RuntimeError("Build failed")

    util.note("")
    util.note("Build complete!")


def upload_firmware(port=None):
    """Upload firmware via sysex"""
    util.note("")
    util.note("=" * 60)
    util.note("FIRMWARE UPLOAD")
    util.note("=" * 60)
    util.note("")
    util.note("Please ensure your Deluge is:")
    util.note("  1. Connected via USB")
    util.note("  2. Configured to accept sysex firmware updates")
    util.note("     (Settings > MIDI > Firmware Update > Enabled)")
    util.note("")
    input("Press Enter when ready...")

    util.note("")
    util.note("Uploading firmware via sysex...")
    util.note("(This takes about 1-2 minutes)")
    util.note("")

    args = ["./dbt", "loadfw", "relwithdebinfo"]
    if port is not None:
        args.extend(["-p", str(port)])

    ret = util.run(args)
    if ret != 0:
        raise RuntimeError("Firmware upload failed")

    util.note("")
    util.note("Upload complete! Waiting for Deluge to restart...")
    util.note("(10 seconds)")
    time.sleep(10)


def collect_data(output_file, duration, interval, port=None, verbose=False):
    """Collect memory profile data via MIDI sysex"""
    try:
        import rtmidi
    except ImportError:
        util.install_rtmidi()
        import rtmidi

    import select

    util.note("")
    util.note("=" * 60)
    util.note("MEMORY PROFILE DATA COLLECTION")
    util.note("=" * 60)
    util.note("")

    midiout = rtmidi.MidiOut()
    midiin = rtmidi.MidiIn()

    try:
        outport = util.ensure_midi_port("output", midiout, port)
        inport = util.ensure_midi_port("input ", midiin, port)

        midiout.open_port(outport)
        midiin.open_port(inport)
    except Exception as e:
        util.note(f"ERROR: {e}")
        util.report_available_midi_ports("output", midiout)
        util.report_available_midi_ports("input", midiin)
        raise

    midiin.ignore_types(False, True, True)

    # Enable sysex logging on Deluge (no context manager - keep port open)
    enable_debug = bytearray(
        [
            0xF0,  # sysex start
            0x00,
            0x21,
            0x7B,  # Deluge manufacturer ID
            0x01,  # protocol version
            0x03,  # debug namespace
            0x00,  # sysex logging config command
            0x01,  # enable
            0xF7,  # sysex end
        ]
    )
    midiout.send_message(enable_debug)
    util.note("Enabled sysex debug logging on Deluge")
    time.sleep(1.0)  # Give Deluge time to process

    # Memory stats request command
    # Format: F0 [mfr] [proto] [namespace] [subcommand] [param] F7
    # Handler receives: data[0]=namespace, data[1]=subcommand, data[2]=param
    # Requires len >= 3 and switches on data[1]
    memory_stats_request = bytearray(
        [
            0xF0,  # sysex start
            0x00,
            0x21,
            0x7B,  # Deluge manufacturer ID
            0x01,  # protocol version
            0x03,  # debug namespace (data[0])
            0x03,  # memory stats subcommand (data[1] = case 3)
            0x00,  # padding (data[2])
            0xF7,  # sysex end
        ]
    )

    # Sysex header for debug log messages
    debug_log_header = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x40, 0x00]

    # Collect data
    samples = []
    start_time = time.time()
    last_request = 0

    def check_for_enter():
        """Non-blocking check if Enter was pressed"""
        if sys.stdin.isatty():
            try:
                ready, _, _ = select.select([sys.stdin], [], [], 0)
                if ready:
                    sys.stdin.readline()
                    return True
            except (ValueError, OSError):
                pass
        return False

    def send_request():
        """Send memory stats request"""
        midiout.send_message(memory_stats_request)
        if verbose:
            util.note("  Sent memory stats request")

    def receive_responses(timeout_sec=5.0):
        """Receive and parse memory stats responses"""
        received = []
        deadline = time.time() + timeout_sec
        header_seen = False

        while time.time() < deadline:
            msg_and_dt = midiin.get_message()
            if msg_and_dt:
                msg, _ = msg_and_dt
                # Check for debug log message
                if len(msg) > 8 and msg[0:8] == debug_log_header:
                    try:
                        # Decode message - firmware just masks to 7-bit ASCII
                        text_bytes = bytearray(b & 0x7F for b in msg[8:-1])
                        text = text_bytes.decode("ascii", errors="ignore").strip()

                        if verbose and text:
                            util.note(f"  [DBG] {text}")

                        # Look for CSV memory output: M,region,...
                        if text.startswith("M,"):
                            if text.startswith("M,region"):
                                # Header line
                                header_seen = True
                            else:
                                # Data line
                                parts = text.split(",")
                                if len(parts) >= 7:
                                    received.append(
                                        {
                                            "ts": time.time(),
                                            "region": parts[1],
                                            "total_size": int(parts[2]),
                                            "free_space": int(parts[3]),
                                            "largest_block": int(parts[4]),
                                            "num_allocs": int(parts[5]),
                                            "num_free_blocks": int(parts[6]),
                                        }
                                    )
                    except (ValueError, UnicodeDecodeError, IndexError) as e:
                        if verbose:
                            util.note(f"  [ERR] Parse error: {e}")

                # Stop after we've seen header + 5 regions
                if header_seen and len(received) >= 5:
                    break
            else:
                time.sleep(0.01)

        return received

    # Determine mode
    if duration is not None and duration > 0:
        # Timed mode
        util.note(
            f"Collecting for {duration}s with {interval}s interval... (press Ctrl+C or Enter to stop)"
        )
    else:
        # Continuous mode (run until stopped)
        util.note(
            f"Collecting with {interval}s interval... (press Ctrl+C or Enter to stop)"
        )
        duration = float("inf")  # Run forever

    try:
        while True:
            # Check for Enter key
            if check_for_enter():
                util.note("\nCollection stopped by user (Enter)")
                break

            # Check for duration timeout
            elapsed = time.time() - start_time
            if duration != float("inf") and elapsed >= duration:
                util.note(f"\nCollection completed ({int(duration)}s)")
                break

            # Time for a new request?
            if time.time() - last_request >= interval:
                util.note(f"  [{int(elapsed)}s] Requesting memory stats...")
                send_request()
                new_samples = receive_responses()
                samples.extend(new_samples)
                last_request = time.time()

            time.sleep(0.1)

    except KeyboardInterrupt:
        util.note("\nCollection stopped by user (Ctrl+C)")

    # Disable sysex logging
    disable_debug = bytearray([0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x00, 0xF7])
    midiout.send_message(disable_debug)

    midiout.close_port()
    midiin.close_port()

    # Write CSV
    util.note("")
    if samples:
        with open(output_file, "w", newline="") as f:
            fieldnames = [
                "ts",
                "region",
                "total_size",
                "free_space",
                "largest_block",
                "num_allocs",
                "num_free_blocks",
            ]
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for sample in samples:
                writer.writerow(sample)

        util.note(f"Wrote {len(samples)} samples to {output_file}")
        util.note("")
        util.note("Summary:")

        # Print summary by region
        for sample in samples[-5:]:  # Last snapshot (5 regions)
            region = sample["region"]
            total = sample["total_size"]
            free = sample["free_space"]
            used = total - free
            pct = (used / total * 100) if total > 0 else 0
            util.note(
                f"  {region:12s}: {used:>12,} / {total:>12,} bytes ({pct:5.1f}% used)"
            )
    else:
        util.note("No samples collected!")
        util.note("Make sure firmware was built with ENABLE_MEMORY_PROFILE=ON")

    return samples


def main():
    parser = argparser()

    try:
        args = parser.parse_args()
    except Exception as e:
        util.note(f"ERROR: {e}")
        return 1

    # Default: do everything
    do_all = not (args.build or args.upload or args.collect)

    try:
        if args.build or do_all:
            build_firmware()

        if args.upload or do_all:
            upload_firmware(args.port)

        if args.collect or do_all:
            collect_data(
                args.output, args.duration, args.interval, args.port, args.verbose
            )

    except Exception as e:
        util.note(f"ERROR: {e}")
        return 1

    util.note("")
    util.note("Done!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
