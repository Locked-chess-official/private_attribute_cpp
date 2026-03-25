import re
import requests
from pathlib import Path

ACTION_PATTERN = re.compile(r"([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)@(v\d+)")

def get_latest_version(repo):
    try:
        api = f"https://api.github.com/repos/{repo}/releases/latest"
        r = requests.get(api, timeout=10)
        if r.status_code == 200:
            return r.json().get("tag_name")

        api = f"https://api.github.com/repos/{repo}/tags"
        r = requests.get(api, timeout=10)
        if r.status_code == 200:
            tags = r.json()
            if tags:
                return tags[0].get("name")
    except Exception as e:
        print(f"Error fetching latest version for {repo}: {e}")
    return None

def update_workflow(path):
    changed = False
    try:
        text = path.read_text(encoding='utf-8')
    except Exception as e:
        print(f"Error reading file {path}: {e}")
        return

    def replacer(match):
        nonlocal changed
        repo, current = match.group(1), match.group(2)
        latest = get_latest_version(repo)
        if latest and latest != current:
            print(f"Updating {repo}: {current} → {latest}")
            changed = True
            return f"{repo}@{latest}"
        return match.group(0)

    new_text = ACTION_PATTERN.sub(replacer, text)

    if changed:
        try:
            path.write_text(new_text, encoding='utf-8')
        except Exception as e:
            print(f"Error writing file {path}: {e}")

def main():
    workflows = Path(".github/workflows").glob("*.yml")
    for wf in workflows:
        print(f"Checking {wf}")
        update_workflow(wf)

if __name__ == "__main__":
    main()
