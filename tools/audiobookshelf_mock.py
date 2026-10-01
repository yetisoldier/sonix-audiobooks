#!/usr/bin/env python3
"""Tiny authenticated Audiobookshelf fixture for the Sonix host smoke test."""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs


class Handler(BaseHTTPRequestHandler):
    media_dir: Path
    progress_file: Path
    token: str

    def log_message(self, fmt, *args):
        return

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, status=200):
        self.send_bytes(status, json.dumps(value).encode(), "application/json")

    def authorized(self):
        if self.headers.get("Authorization") == f"Bearer {self.token}":
            return True
        self.send_json({"error": "unauthorized"}, 401)
        return False

    def do_GET(self):
        if not self.authorized():
            return
        path = urlparse(self.path).path
        with self.progress_file.with_suffix(".requests").open("a") as log:
            log.write(self.path + "\n")
        if path == "/api/libraries/mock-library/filterdata":
            self.send_json({"authors": [{"id": "author-1", "name": "Test Author"}],
                            "series": [{"id": "series-1", "name": "Fixture Series"}]})
            return
        if path == "/api/libraries/mock-library/search":
            query = parse_qs(urlparse(self.path).query).get("q", [""])[0]
            matches = []
            if query.lower() in "the mock journey":
                matches = [{"libraryItem": {"id": "mock-book", "media": {
                    "duration": 20, "metadata": {"title": "The Mock Journey", "authorName": "Test Author"}
                }}}]
            self.send_json({"book": matches})
            return
        if path == "/api/me/progress/mock-book":
            if self.progress_file.with_suffix(".offline").exists():
                self.send_json({"error": "offline test"}, 503)
                return
            value = json.loads(self.progress_file.read_text()) if self.progress_file.exists() else {
                "currentTime": 13, "isFinished": False, "lastUpdate": 1700000000000,
            }
            self.send_json({**value, "libraryItemId": "mock-book"})
            return
        if path == "/api/libraries":
            self.send_json({"libraries": [{"id": "mock-library", "name": "Mock Audiobooks", "mediaType": "book"}]})
            return
        if path == "/api/libraries/mock-library/items":
            self.send_json({
                "total": 1,
                "results": [{
                    "id": "mock-book",
                    "media": {
                        "duration": 20,
                        "numTracks": 2,
                        "coverPath": "/fixture/cover.jpg",
                        "metadata": {
                            "title": "The Mock Journey",
                            "authorName": "Test Author",
                            "seriesName": "Fixture Series",
                        },
                    },
                    "userMediaProgress": {"currentTime": 13, "isFinished": False},
                }],
            })
            return
        if path == "/api/items/mock-book":
            self.send_json({
                "id": "mock-book",
                "media": {
                    "duration": 20,
                    "coverPath": "/fixture/cover.jpg",
                    "metadata": {
                        "title": "The Mock Journey",
                        "authorName": "Test Author",
                        "description": "A downloaded fixture used to prove resume import.",
                        "seriesName": "Fixture Series",
                        "series": [{"name": "Fixture Series", "sequence": "2"}],
                    },
                    "tracks": [
                        {
                            "index": 1,
                            "startOffset": 0,
                            "duration": 10,
                            "title": "Part One",
                            "contentUrl": "/api/items/mock-book/file/1",
                            "mimeType": "audio/mpeg",
                            "metadata": {"filename": "Part One.mp3"},
                        },
                        {
                            "index": 2,
                            "startOffset": 10,
                            "duration": 10,
                            "title": "Part Two",
                            "contentUrl": "/api/items/mock-book/file/2",
                            "mimeType": "audio/mpeg",
                            "metadata": {"filename": "Part Two.mp3"},
                        },
                    ],
                },
                "userMediaProgress": {"currentTime": 13, "isFinished": False, "lastUpdate": 1700000000000},
            })
            return
        if path in ("/api/items/mock-book/file/1", "/api/items/mock-book/file/2"):
            name = "part1.mp3" if path.endswith("/1") else "part2.mp3"
            self.send_bytes(200, (self.media_dir / name).read_bytes(), "audio/mpeg")
            return
        if path == "/api/items/mock-book/cover":
            self.send_bytes(200, (self.media_dir / "cover.jpg").read_bytes(), "image/jpeg")
            return
        self.send_json({"error": "not found"}, 404)

    def do_PATCH(self):
        if not self.authorized():
            return
        if urlparse(self.path).path != "/api/me/progress/mock-book":
            self.send_json({"error": "not found"}, 404)
            return
        size = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(size)
        value = json.loads(body or b"{}")
        value.setdefault("lastUpdate", int(time.time() * 1000))
        # A reversible outage without killing the authenticated fixture server.
        if self.progress_file.with_suffix(".offline").exists():
            self.send_json({"error": "offline test"}, 503)
            return
        self.progress_file.write_text(json.dumps(value), encoding="utf-8")
        self.send_json(value)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--media-dir", type=Path, required=True)
    parser.add_argument("--port-file", type=Path, required=True)
    parser.add_argument("--progress-file", type=Path, required=True)
    parser.add_argument("--token", default="test-token")
    args = parser.parse_args()
    Handler.media_dir = args.media_dir
    Handler.progress_file = args.progress_file
    Handler.token = args.token
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    args.port_file.write_text(str(server.server_port), encoding="ascii")
    server.serve_forever()


if __name__ == "__main__":
    main()
