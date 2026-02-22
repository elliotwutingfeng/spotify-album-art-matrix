"""Run this script to obtain a Spotify refresh token."""

import typing
import base64
import getpass
import json
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer


class RequestHandler(BaseHTTPRequestHandler):
    auth_code: str | None = None

    def do_GET(self):
        try:
            values = urllib.parse.parse_qs(
                urllib.parse.urlparse(self.path).query,
                keep_blank_values=False,
            ).get("code", [])
            RequestHandler.auth_code = values[0] if values else None
        except Exception:
            RequestHandler.auth_code = None
        self.send_response(200 if RequestHandler.auth_code else 400)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(
            b"<h2>Done! You can close this tab.</h2>"
            if RequestHandler.auth_code
            else b"<h2>Authorization failed.</h2>"
        )

    def log_message(self, format: str, *args: typing.Any):
        pass


if __name__ == "__main__":
    CLIENT_ID = input("Client ID: ").strip()
    CLIENT_SECRET = getpass.getpass("Client Secret (hidden): ").strip()
    REDIRECT_URI = "http://127.0.0.1:3000/callback"

    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(
        {
            "response_type": "code",
            "client_id": CLIENT_ID,
            "scope": "user-read-currently-playing",
            "redirect_uri": REDIRECT_URI,
        }
    )
    print(
        f"Opening browser for Spotify authorization...\nIf it does not open, visit:\n  {auth_url}\n"
    )
    webbrowser.open(auth_url)
    with HTTPServer(("127.0.0.1", 3000), RequestHandler) as server:
        server.handle_request()

    if not RequestHandler.auth_code:
        raise SystemExit("Failed to capture authorization code.")

    url = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode(
            {
                "grant_type": "authorization_code",
                "code": RequestHandler.auth_code,
                "redirect_uri": REDIRECT_URI,
            }
        ).encode(),
        headers={
            "Authorization": f"Basic {base64.b64encode(f'{CLIENT_ID}:{CLIENT_SECRET}'.encode()).decode()}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    with urllib.request.urlopen(url, timeout=10) as response:
        token_info = json.load(response)
    if isinstance(token_info, dict) and (
        refresh_token := token_info.get("refresh_token")
    ):
        print(f"\nREFRESH_TOKEN:\n  {refresh_token}")
    else:
        raise SystemExit("No refresh token in response.")
