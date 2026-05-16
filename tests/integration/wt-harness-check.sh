#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

mkdir -p "$tmp_dir/models/openai" "$tmp_dir/models/google" "$tmp_dir/models/qwen" "$tmp_dir/bin"

cat > "$tmp_dir/models/openai/codex.yaml" <<'YAML'
model_id: openai/gpt-5.3-codex
status: active
cli_tool: codex
YAML

cat > "$tmp_dir/models/google/gemini.yaml" <<'YAML'
model_id: google/gemini-pro
status: active
cli_tool: gemini
YAML

cat > "$tmp_dir/models/qwen/qwen.yaml" <<'YAML'
model_id: qwen/qwen-max
status: active
cli_tool: qwen
YAML

for tool in claude codex gemini ollama; do
cat > "$tmp_dir/bin/$tool" <<SH
#!/usr/bin/env sh
echo "$tool test"
SH
chmod +x "$tmp_dir/bin/$tool"
done

PATH="$tmp_dir/bin:$PATH" "$repo_root/bin/wt-harness-check" --models-dir "$tmp_dir/models" --json > "$tmp_dir/report.json"

grep -q '"tool":"claude","state":"launchable"' "$tmp_dir/report.json"
grep -q '"tool":"codex","state":"launchable"' "$tmp_dir/report.json"
grep -q '"tool":"ollama","state":"launchable"' "$tmp_dir/report.json"
grep -q '"modelId":"openai/gpt-5.3-codex".*"state":"launchable"' "$tmp_dir/report.json"
grep -q '"modelId":"google/gemini-pro".*"state":"launchable"' "$tmp_dir/report.json"
grep -q '"modelId":"qwen/qwen-max".*"state":"missing"' "$tmp_dir/report.json"
grep -q '"missingActiveCli": true' "$tmp_dir/report.json"

if PATH="$tmp_dir/bin:$PATH" "$repo_root/bin/wt-harness-check" --models-dir "$tmp_dir/models" --strict >/dev/null; then
    echo "expected --strict to fail when qwen is missing" >&2
    exit 1
fi

cat > "$tmp_dir/bin/qwen" <<'SH'
#!/usr/bin/env sh
echo "qwen test"
SH
chmod +x "$tmp_dir/bin/qwen"

PATH="$tmp_dir/bin:$PATH" "$repo_root/bin/wt-harness-check" --models-dir "$tmp_dir/models" --strict >/dev/null
