#!/usr/bin/env bash
# eden-kledd: put a finished lab build into the private "Lab" draft release (tag lab-latest),
# replacing that platform's previous files, so the newest lab build downloads instantly.
# Drafts are only visible to people with write access to the repo. Older builds stay in Actions.
#
# usage: lab-release.sh "<label shown next to the files>" "<regex of the files to replace>" <file>...
#   e.g. lab-release.sh "Windows #144 [...]" '-windows\.zip$' eden-kledd-lab-144-...-windows.zip
# needs: GH_TOKEN with contents: write, GITHUB_REPOSITORY
set -euo pipefail

repo="$GITHUB_REPOSITORY"
label="$1"
replace_regex="$2"
shift 2

id=$(gh api "repos/$repo/releases?per_page=100" \
    --jq '[.[] | select(.draft and .tag_name == "lab-latest")][0].id // empty')
if [ -z "$id" ]; then
    id=$(gh api -X POST "repos/$repo/releases" \
        -f tag_name=lab-latest -f target_commitish=main -F draft=true \
        -f name="Lab (latest test builds)" \
        -f body="Private draft, only visible to you. Every build of a branch other than main replaces its files here automatically; the file names and labels say which build it is. Older builds stay in the Actions tab." \
        --jq .id)
    echo "created the Lab draft release ($id)"
fi

# This platform's previous files (the names change with every build). The regex goes inside a jq
# string literal, so its backslashes are doubled.
jq_regex=${replace_regex//\\/\\\\}
for old in $(gh api "repos/$repo/releases/$id/assets?per_page=100" \
        --jq ".[] | select(.name | test(\"^eden-kledd-lab-\")) | select(.name | test(\"$jq_regex\")) | .id"); do
    gh api -X DELETE "repos/$repo/releases/assets/$old"
    echo "removed the previous file ($old)"
done

encoded_label=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$label")
for file in "$@"; do
    name=$(basename "$file")
    curl -fsS -X POST \
        -H "Authorization: Bearer $GH_TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -H "Content-Type: application/octet-stream" \
        --data-binary @"$file" \
        "https://uploads.github.com/repos/$repo/releases/$id/assets?name=$name&label=$encoded_label" \
        > /dev/null
    echo "uploaded $name ($label)"
done
