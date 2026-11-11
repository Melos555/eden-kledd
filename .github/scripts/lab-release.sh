#!/usr/bin/env bash
# eden-kledd: put a finished lab build into the private "Lab" draft release (tag lab-latest),
# replacing the previous file of the same name, so the newest lab build downloads instantly.
# Drafts are only visible to people with write access to the repo. Older builds stay in Actions.
#
# usage: lab-release.sh "<label shown next to the file>" <file>...
# needs: GH_TOKEN with contents: write, GITHUB_REPOSITORY
set -euo pipefail

repo="$GITHUB_REPOSITORY"
label="$1"
shift

id=$(gh api "repos/$repo/releases?per_page=100" \
    --jq '[.[] | select(.draft and .tag_name == "lab-latest")][0].id // empty')
if [ -z "$id" ]; then
    id=$(gh api -X POST "repos/$repo/releases" \
        -f tag_name=lab-latest -f target_commitish=main -F draft=true \
        -f name="Lab (latest test builds)" \
        -f body="Private draft, only visible to you. Every build of a branch other than main replaces its file here automatically; the label next to each file says which build it is. Older builds stay in the Actions tab." \
        --jq .id)
    echo "created the Lab draft release ($id)"
fi

encoded_label=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$label")
for file in "$@"; do
    name=$(basename "$file")
    old=$(gh api "repos/$repo/releases/$id/assets?per_page=100" \
        --jq ".[] | select(.name == \"$name\") | .id")
    if [ -n "$old" ]; then
        gh api -X DELETE "repos/$repo/releases/assets/$old"
    fi
    curl -fsS -X POST \
        -H "Authorization: Bearer $GH_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -H "Content-Type: application/octet-stream" \
        --data-binary @"$file" \
        "https://uploads.github.com/repos/$repo/releases/$id/assets?name=$name&label=$encoded_label" \
        > /dev/null
    echo "uploaded $name ($label)"
done
