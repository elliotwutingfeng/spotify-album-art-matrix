"""Get album image URL of the currently playing track on Spotify using Spotify Web API."""

import tomllib
import base64
import urllib.error
import urllib.parse
import urllib.request
import json
import pathlib

with open(pathlib.Path(__file__).parent / "env.toml", "rb") as f:
    env = tomllib.load(f)

CLIENT_ID = env["CLIENT_ID"]
CLIENT_SECRET = env["CLIENT_SECRET"]
REDIRECT_URI = env["REDIRECT_URI"]
REFRESH_TOKEN = env["REFRESH_TOKEN"]


def get_access_token():
    """Return a new access token using refresh token."""
    url = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode(
            {"grant_type": "refresh_token", "refresh_token": REFRESH_TOKEN}
        ).encode(),
        headers={
            "Authorization": f"Basic {base64.b64encode(f'{CLIENT_ID}:{CLIENT_SECRET}'.encode()).decode()}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            response_data = response.read()
    except urllib.error.HTTPError as e:
        print(f"Error refreshing token: {e.code}\n{e.read().decode()}")
        return None

    if response.status != 200:
        print(f"Error refreshing token: {response.status}")
        print(response_data.decode())
        return None

    token_info = json.loads(response_data)
    if isinstance(token_info, dict) and (
        access_token := token_info.get("access_token")
    ):
        return access_token
    return None


def get_album_image_of_current_track():
    """Return the smallest album image URL of the currently playing Spotify track."""
    access_token = get_access_token()
    if not access_token:
        return None

    url = urllib.request.Request(
        "https://api.spotify.com/v1/me/player/currently-playing",
        headers={"Authorization": f"Bearer {access_token}"},
    )
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            if response.status == 204:
                print("No song is currently playing.")
                return None
            response_data = response.read()
    except urllib.error.HTTPError as e:
        print(f"Error getting current track: {e.code}\n{e.read().decode()}")
        return None

    current_track = json.loads(response_data)

    if not isinstance(current_track, dict):
        print("Unexpected response format.")
        return None

    if not current_track.get("is_playing"):
        print("No song is currently playing.")
        return None

    try:
        images = current_track["item"]["album"]["images"]
        if not isinstance(images, list):
            images = []
    except KeyError, TypeError:
        images = []
    return (
        min(images, key=lambda img: img.get("width", float("inf")))["url"]
        if images
        else None
    )


if __name__ == "__main__":
    smallest_album_image = get_album_image_of_current_track()
    print(smallest_album_image if smallest_album_image else "No album image available.")
