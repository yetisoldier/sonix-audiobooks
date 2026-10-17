#!/usr/bin/env python3
"""Generate disposable tags and exercise the real metadata parser, not a mock."""
import pathlib
import struct
import subprocess
import sys
import tempfile


def syncsafe(n):
    return bytes((n >> shift) & 127 for shift in (21, 14, 7, 0))


def text(value, encoding):
    return value.encode({0: "latin1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}[encoding])


def frame(key, data, version=3):
    if version == 2:
        return key.encode() + len(data).to_bytes(3, "big") + data
    size = syncsafe(len(data)) if version == 4 else struct.pack(">I", len(data))
    return key.encode() + size + b"\0\0" + data


def id3(path, flag="1", encoding=3, custom=False, version=3):
    def tag(key, value):
        return frame(key, bytes([encoding]) + text(value, encoding), version)

    parts = [tag("TP1" if version == 2 else "TPE1", "Narrator"),
             tag("TP2" if version == 2 else "TPE2", "Author")]
    if custom:
        delimiter = b"\0\0" if encoding in (1, 2) else b"\0"
        parts.append(frame("TXXX", bytes([encoding]) + text("COMPILATION", encoding)
                           + delimiter + text(flag, encoding), version))
    else:
        parts.append(tag("TCP" if version == 2 else "TCMP", flag))
    payload = b"".join(parts)
    path.write_bytes(b"ID3" + bytes([version, 0, 0]) + syncsafe(len(payload)) + payload)


def check(probe, path, flag, shown, series="", part="", summary=""):
    result = subprocess.run([probe, str(path)], text=True, capture_output=True, check=True)
    expected = [str(int(flag)), shown, "Narrator", "Author", series, part, summary]
    actual = result.stdout.splitlines()
    assert actual == expected, (path.name, actual, expected)


def main():
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    count = 0
    with tempfile.TemporaryDirectory(prefix="sonix-metadata-") as directory:
        root = pathlib.Path(directory)
        for version in (2, 3, 4):
            for encoding in (0, 1, 2, 3):
                if version == 2 and encoding not in (0, 1):
                    continue
                for custom in (False, True):
                    if version == 2 and custom:
                        continue
                    for flag, enabled in (("1", True), ("true", True), (" YES ", True),
                                          ("0", False), ("false", False), ("", False),
                                          ("10", False), ("yesterday", False)):
                        path = root / f"id3-{version}-{encoding}-{custom}-{count}.mp3"
                        id3(path, flag, encoding, custom, version)
                        check(probe, path, enabled, "Narrator" if enabled else "Author")
                        count += 1
        for suffix, codec in (("flac", "flac"), ("ogg", "libvorbis"),
                              ("opus", "libopus"), ("wv", "wavpack"), ("m4a", "aac")):
            for flag in ("0", "1"):
                path = root / f"compilation-{flag}.{suffix}"
                subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                                "sine=duration=0.15", "-c:a", codec,
                                "-metadata", "artist=Narrator", "-metadata", "album_artist=Author",
                                "-metadata", f"compilation={flag}", str(path)], check=True)
                check(probe, path, flag == "1", "Narrator" if flag == "1" else "Author")
                count += 1
        # APE tag-only fixture: this suite tests the tag reader, not APE audio decoding.
        path = root / "tags.ape"
        items = {"Artist": "Narrator", "Album Artist": "Author", "Compilation": "1"}
        body = b"".join(struct.pack("<II", len(v), 0) + k.encode() + b"\0" + v.encode()
                        for k, v in items.items())
        footer = b"APETAGEX" + struct.pack("<IIII", 2000, len(body) + 32, len(items), 0) + b"\0" * 8
        path.write_bytes(b"\0" * 32 + body + footer)
        check(probe, path, True, "Narrator")
        count += 1
        book = root / "book.mp3"
        id3(book, "0")
        pathlib.Path(str(book) + ".tags").write_text(
            "album=Fixture Book\nartist=Narrator\nalbum_artist=Author\nseries=Fixture Series\nseries_part=2\n")
        pathlib.Path(str(book) + ".description").write_text("<p>Book summary.</p>")
        check(probe, book, False, "Author", "Fixture Series", "2", "Book summary.")
        count += 1
    print(f"Metadata regression passed: {count} tag/sidecar fixtures plus artist fallbacks (ASan/UBSan)")


if __name__ == "__main__":
    main()
