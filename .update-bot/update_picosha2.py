import requests
import hashlib
import os

URL = "https://raw.githubusercontent.com/okdshin/PicoSHA2/master/picosha2.h"
LOCAL = "picosha2.h"

def get_sha256(data):
    return hashlib.sha256(data).hexdigest()

def main():
    print("Checking picosha2.h...")

    r = requests.get(URL, timeout=10)
    r.raise_for_status()
    remote_data = r.content

    if not os.path.exists(LOCAL):
        with open(LOCAL, "wb") as f:
            f.write(remote_data)
        print("picosha2.h created")
        return

    with open(LOCAL, "rb") as f:
        local_data = f.read()

    if get_sha256(local_data) == get_sha256(remote_data):
        print("No change in picosha2.h")
        return

    with open(LOCAL, "wb") as f:
        f.write(remote_data)

    print("picosha2.h updated")

if __name__ == "__main__":
    main()
