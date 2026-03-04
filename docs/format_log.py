#!/usr/bin/env python3
"""
Convert Claude Code JSONL conversation logs into a readable HTML document.

Usage: python3 format_log.py [-o output.html] logfile1.jsonl [logfile2.jsonl ...]

Strips filesystem paths, hides thinking blocks. Tool calls and results are
grouped into collapsible "work" sections so the reader sees only the
conversation by default and can expand to see what Claude did.
"""

import json
import sys
import os
import re
import html
import argparse

# paths and identifiers to strip from output
PATH_REPLACEMENTS = [
    (re.compile(r'/Users/[^/]+/Documents/Code/Github/lang/cj/lisa/'), 'lisa/'),
    (re.compile(r'/Users/[^/]+/Documents/Code/Github/lang/cj/'), 'cj/'),
    (re.compile(r'/Users/[^/]+/\.claude/[^\s"\']+'), '<claude-internal>'),
    (re.compile(r'/Users/[^/]+/'), '~/'),
    (re.compile(r'/home/[^/]+/'), '~/'),
    (re.compile(r'/tmp/lisa_bench_\w+\.lisa'), '<benchmark>'),
    # temp task output paths
    (re.compile(r'/private/tmp/claude-\d+/[^\s]+'), '<task-output>'),
    # claude project slugs containing username-derived paths
    (re.compile(r'-Users-[A-Za-z0-9_]+-Documents-[^\s"\'*/]+'), '<project>'),
    # session UUIDs (8-4-4-4-12 hex)
    (re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}'), '<uuid>'),
    # username from ls -la output (e.g. "veitheller  staff")
    (re.compile(r'(?<=\d )\S+(?=\s+staff\s)'), 'user'),
]

# additional words to scrub (usernames, hostnames, etc.)
SCRUB_WORDS = ['veitheller']

# session titles (order matches chronological sort)
SESSION_TITLES = [
    "exploring cj",
    "building lisa: bytecode vm",
    "tail call optimization",
    "whole-function jit",
    "jit rewrite: register cache + inline fast paths",
    "fibers and channels",
    "string primitives + json parser",
    "def as local + bug fixes + docs + gc fixes",
]


def sanitize(text):
    """Strip filesystem paths and sensitive words."""
    for pattern, replacement in PATH_REPLACEMENTS:
        text = pattern.sub(replacement, text)
    for word in SCRUB_WORDS:
        text = text.replace(word, '<user>')
    return text


def escape(text):
    """HTML-escape and sanitize."""
    return sanitize(html.escape(text))


def format_code_block(text, lang=''):
    """Wrap text in a <pre><code> block."""
    return f'<pre><code class="{lang}">{escape(text)}</code></pre>'


def render_markdown(text):
    """Render simple markdown to HTML."""
    text = sanitize(text)
    lines = text.split('\n')
    out = []
    in_code = False
    code_buf = []
    code_lang = ''
    for line in lines:
        if line.startswith('```') and not in_code:
            in_code = True
            code_lang = line[3:].strip()
            code_buf = []
        elif line.startswith('```') and in_code:
            in_code = False
            out.append(format_code_block('\n'.join(code_buf), code_lang))
        elif in_code:
            code_buf.append(line)
        else:
            escaped = html.escape(line)
            escaped = re.sub(r'`([^`]+)`', r'<code>\1</code>', escaped)
            escaped = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', escaped)
            if escaped.startswith('### '):
                escaped = f'<h5>{escaped[4:]}</h5>'
            elif escaped.startswith('## '):
                escaped = f'<h4>{escaped[3:]}</h4>'
            elif escaped.startswith('# '):
                escaped = f'<h3>{escaped[2:]}</h3>'
            elif escaped.startswith('|'):
                escaped = f'<span class="table-line">{escaped}</span>'
            elif escaped.strip() == '---' or escaped.strip() == '-----':
                escaped = ''
            else:
                if escaped.strip():
                    escaped = f'<p>{escaped}</p>'
                else:
                    escaped = ''
            out.append(escaped)
    if in_code:
        out.append(format_code_block('\n'.join(code_buf), code_lang))
    return '\n'.join(out)


def render_tool_use(block):
    """Render a tool_use block as a collapsible item."""
    name = block.get('name', '?')
    inp = block.get('input', {})
    # build a short summary
    desc = inp.get('description', '')
    if name == 'Read':
        path = sanitize(inp.get('file_path', ''))
        summary_extra = f' &mdash; {html.escape(path)}'
    elif name == 'Edit':
        path = sanitize(inp.get('file_path', ''))
        summary_extra = f' &mdash; {html.escape(path)}'
    elif name == 'Write':
        path = sanitize(inp.get('file_path', ''))
        summary_extra = f' &mdash; {html.escape(path)}'
    elif name == 'Bash' and desc:
        summary_extra = f' &mdash; {html.escape(sanitize(desc))}'
    elif name == 'Grep':
        pat = inp.get('pattern', '')
        summary_extra = f' &mdash; {html.escape(sanitize(pat))}'
    elif name == 'Glob':
        pat = inp.get('pattern', '')
        summary_extra = f' &mdash; {html.escape(sanitize(pat))}'
    elif name == 'Agent':
        desc2 = inp.get('description', inp.get('prompt', '')[:60])
        summary_extra = f' &mdash; {html.escape(sanitize(desc2))}'
    else:
        summary_extra = ''

    inp_str = sanitize(json.dumps(inp, indent=2, ensure_ascii=False))
    if len(inp_str) > 2000:
        inp_str = inp_str[:2000] + '\n... (truncated)'
    return (
        f'<details class="tool-call">'
        f'<summary><span class="tool-name">{html.escape(name)}</span>'
        f'{summary_extra}</summary>'
        f'<pre><code>{html.escape(inp_str)}</code></pre>'
        f'</details>'
    )


def render_tool_result(block):
    """Render a tool_result block as a collapsible item."""
    content = block.get('content', '')
    if isinstance(content, list):
        parts = []
        for c in content:
            if c.get('type') == 'text':
                t = c['text']
                t = re.sub(r'<system-reminder>.*?</system-reminder>', '', t, flags=re.DOTALL)
                t = t.strip()
                if t:
                    parts.append(t)
        content = '\n'.join(parts)
    elif isinstance(content, str):
        content = re.sub(r'<system-reminder>.*?</system-reminder>', '', content, flags=re.DOTALL).strip()
    if not content:
        return ''
    content = sanitize(content)
    if len(content) > 3000:
        content = content[:3000] + '\n... (truncated)'
    return (
        f'<details class="tool-result">'
        f'<summary><span class="result-label">result</span></summary>'
        f'<pre><code>{html.escape(content)}</code></pre>'
        f'</details>'
    )


def summarize_tool_group(tool_blocks):
    """Build a one-line summary for a group of tool operations."""
    ops = []
    for kind, block in tool_blocks:
        if kind == 'tool_use':
            name = block.get('name', '?')
            ops.append(name)
    # compress: "Read, Read, Read" -> "Read x3"
    from collections import Counter
    counts = Counter(ops)
    parts = []
    for op, count in counts.items():
        if count > 1:
            parts.append(f'{op}&nbsp;&times;{count}')
        else:
            parts.append(op)
    return ', '.join(parts) if parts else 'work'


def process_session(filepath, session_idx):
    """Process one JSONL file into a list of (type, html) tuples.

    Types: 'text-user', 'text-assistant', 'work'
    Work blocks group consecutive tool_use/tool_result pairs.
    """
    # First pass: collect raw blocks with roles
    raw = []  # list of (role, block_type, block_or_text)

    with open(filepath) as f:
        for line in f:
            obj = json.loads(line)
            msg_type = obj.get('type')

            if msg_type == 'user':
                msg = obj.get('message', {})
                content = msg.get('content', '')
                if isinstance(content, list):
                    for block in content:
                        if block.get('type') == 'text':
                            text = block['text'].strip()
                            if text.startswith('[Request interrupted'):
                                continue
                            text = re.sub(r'<system-reminder>.*?</system-reminder>', '', text, flags=re.DOTALL).strip()
                            if text:
                                raw.append(('user', 'text', text))
                        elif block.get('type') == 'tool_result':
                            raw.append(('user', 'tool_result', block))
                elif isinstance(content, str):
                    text = re.sub(r'<system-reminder>.*?</system-reminder>', '', content, flags=re.DOTALL).strip()
                    if text:
                        raw.append(('user', 'text', text))

            elif msg_type == 'assistant':
                msg = obj.get('message', {})
                content = msg.get('content', [])
                if isinstance(content, list):
                    for block in content:
                        btype = block.get('type', '')
                        if btype == 'text':
                            text = block.get('text', '').strip()
                            if text:
                                raw.append(('assistant', 'text', text))
                        elif btype == 'tool_use':
                            raw.append(('assistant', 'tool_use', block))
                        elif btype == 'thinking':
                            pass  # skip
                elif isinstance(content, str):
                    text = content.strip()
                    if text:
                        raw.append(('assistant', 'text', text))

    # Second pass: group into conversation messages and work blocks
    messages = []
    i = 0
    while i < len(raw):
        role, btype, data = raw[i]

        if btype == 'text':
            messages.append((f'text-{role}', render_markdown(data)))
            i += 1
        elif btype in ('tool_use', 'tool_result'):
            # collect consecutive tool blocks (across assistant/user boundaries)
            tool_blocks = []
            while i < len(raw) and raw[i][1] in ('tool_use', 'tool_result'):
                tool_blocks.append((raw[i][1], raw[i][2]))
                i += 1
            # render them as a single collapsible work section
            inner_parts = []
            for kind, block in tool_blocks:
                if kind == 'tool_use':
                    inner_parts.append(render_tool_use(block))
                elif kind == 'tool_result':
                    rendered = render_tool_result(block)
                    if rendered:
                        inner_parts.append(rendered)
            if inner_parts:
                summary = summarize_tool_group(tool_blocks)
                messages.append(('work', summary, '\n'.join(inner_parts), ''))
        else:
            i += 1

    # Third pass: fold assistant narration into adjacent work groups.
    # If an assistant text is immediately followed by a work group, merge it
    # as the work group's description (collapsed by default).
    # If an assistant text is between two work groups, merge it into the next one.
    merged = []
    i = 0
    while i < len(messages):
        msg = messages[i]

        if (msg[0] == 'text-assistant' and
                i + 1 < len(messages) and messages[i + 1][0] == 'work'):
            # fold this text into the next work group as its description
            work = messages[i + 1]
            merged.append(('work', work[1], work[2], msg[1]))
            i += 2
        else:
            merged.append(msg)
            i += 1

    return merged


def generate_html(sessions):
    """Generate the full HTML document."""
    css = """
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
        font-family: 'Berkeley Mono', 'IBM Plex Mono', 'JetBrains Mono', monospace;
        font-size: 14px;
        line-height: 1.6;
        background: #0d1117;
        color: #c9d1d9;
    }
    .layout {
        display: flex;
        min-height: 100vh;
    }
    .sidebar {
        position: fixed;
        top: 0;
        left: 0;
        width: 220px;
        height: 100vh;
        overflow-y: auto;
        background: #010409;
        border-right: 1px solid #21262d;
        padding: 1em 0.8em;
        z-index: 10;
    }
    .sidebar h1 {
        font-size: 1em;
        color: #f0f6fc;
        margin-bottom: 0.8em;
        padding-bottom: 0.5em;
        border-bottom: 1px solid #21262d;
    }
    .sidebar a {
        display: block;
        color: #8b949e;
        text-decoration: none;
        padding: 0.3em 0.4em;
        border-radius: 4px;
        font-size: 0.8em;
        line-height: 1.4;
        margin: 0.1em 0;
    }
    .sidebar a:hover { background: #161b22; color: #c9d1d9; }
    .sidebar a.active { color: #58a6ff; background: #161b22; }
    .sidebar .nav-num { color: #484f58; margin-right: 0.2em; }
    .main {
        margin-left: 220px;
        max-width: 900px;
        padding: 2em 1.5em;
        flex: 1;
    }
    /* hide sidebar on narrow screens, show inline nav instead */
    @media (max-width: 800px) {
        .sidebar { display: none; }
        .main { margin-left: 0; padding: 1em; }
        .mobile-nav { display: block; }
    }
    @media (min-width: 801px) {
        .mobile-nav { display: none; }
    }
    .mobile-nav { margin: 1em 0 2em; }
    .mobile-nav a {
        display: inline-block;
        color: #58a6ff;
        text-decoration: none;
        margin: 0.2em 0;
        padding: 0.2em 0.5em;
        border: 1px solid #21262d;
        border-radius: 4px;
        font-size: 0.85em;
    }
    h1 { color: #f0f6fc; margin: 0.5em 0; font-size: 1.4em; }
    h2 { color: #f0f6fc; margin: 1.5em 0 0.5em; font-size: 1.2em;
         border-bottom: 1px solid #21262d; padding-bottom: 0.3em; }
    h3 { color: #e6edf3; margin: 0.5em 0; font-size: 1.1em; }
    h4 { color: #e6edf3; margin: 0.4em 0; font-size: 1em; }
    h5 { color: #e6edf3; margin: 0.3em 0; font-size: 0.95em; }
    p { margin: 0.4em 0; }
    a { color: #58a6ff; }
    .intro { color: #8b949e; margin-bottom: 2em; }
    .message {
        margin: 0.8em 0;
        padding: 0.8em 1em;
        border-radius: 6px;
        border-left: 3px solid transparent;
    }
    .message.user {
        background: #161b22;
        border-left-color: #58a6ff;
    }
    .message.assistant {
        background: #0d1117;
        border-left-color: #3fb950;
    }
    .message-label {
        font-size: 0.75em;
        text-transform: uppercase;
        letter-spacing: 0.1em;
        margin-bottom: 0.3em;
    }
    .message.user .message-label { color: #58a6ff; }
    .message.assistant .message-label { color: #3fb950; }
    pre {
        background: #161b22;
        border: 1px solid #21262d;
        border-radius: 4px;
        padding: 0.6em;
        overflow-x: auto;
        margin: 0.4em 0;
        font-size: 0.9em;
    }
    code {
        font-family: inherit;
        font-size: 0.95em;
    }
    p code, li code {
        background: #161b22;
        padding: 0.1em 0.3em;
        border-radius: 3px;
        border: 1px solid #21262d;
    }
    details { margin: 0.3em 0; }
    details > summary {
        cursor: pointer;
        padding: 0.2em 0.4em;
        border-radius: 3px;
        font-size: 0.85em;
    }
    details > summary:hover { background: #21262d; }
    .work-group {
        margin: 0.4em 0;
        border: 1px solid #21262d;
        border-radius: 6px;
        background: #0d1117;
    }
    .work-group > summary {
        cursor: pointer;
        padding: 0.5em 0.8em;
        font-size: 0.85em;
        color: #8b949e;
        border-radius: 6px;
        user-select: none;
    }
    .work-group > summary:hover { background: #161b22; }
    .work-group > summary .work-icon { color: #d2a8ff; margin-right: 0.3em; }
    .work-group-inner {
        padding: 0.4em 0.8em;
        border-top: 1px solid #21262d;
    }
    .work-desc {
        color: #c9d1d9;
        padding: 0.4em 0;
        margin-bottom: 0.4em;
        border-bottom: 1px solid #21262d;
    }
    .tool-call summary { color: #d2a8ff; }
    .tool-result summary { color: #484f58; }
    .tool-name { font-weight: bold; }
    .result-label { font-style: italic; }
    .table-line {
        display: block;
        font-family: inherit;
        white-space: pre;
        color: #8b949e;
    }
    strong { color: #f0f6fc; }
    .session-header {
        background: #161b22;
        border: 1px solid #21262d;
        border-radius: 6px;
        padding: 0.8em 1em;
        margin: 2em 0 1em;
    }
    .session-header h2 { border: none; margin: 0; padding: 0; }
    .session-number { color: #484f58; }
    """

    sidebar_links = []
    mobile_links = []
    for i, (title, _) in enumerate(sessions):
        sidebar_links.append(
            f'<a href="#session-{i+1}" data-session="{i+1}">'
            f'<span class="nav-num">{i+1}.</span> {html.escape(title)}</a>'
        )
        mobile_links.append(
            f'<a href="#session-{i+1}">{i+1}. {html.escape(title)}</a>'
        )

    parts = [f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>building lisa: a conversation log</title>
<style>{css}</style>
</head>
<body>
<div class="layout">

<nav class="sidebar">
<h1>building lisa</h1>
{chr(10).join(sidebar_links)}
</nav>

<div class="main">

<h1>building lisa</h1>
<div class="intro">
<p>a conversation log of building a programming language with a jit compiler.</p>
<p>lisa is a lisp with closures, tail calls, fibers, channels, and a jit backend,
built on top of <a href="https://github.com/cj-lang/cj">cj</a>,
a minimal jit framework for c.</p>
<p>8 sessions over 4 days. all code was written by claude.</p>
</div>

<nav class="mobile-nav">
{' '.join(mobile_links)}
</nav>
"""]

    for i, (title, messages) in enumerate(sessions):
        parts.append(
            f'<div class="session-header" id="session-{i+1}">'
            f'<h2><span class="session-number">session {i+1}.</span> {html.escape(title)}</h2>'
            f'</div>'
        )

        for msg in messages:
            if msg[0] == 'text-user':
                parts.append(
                    f'<div class="message user">'
                    f'<div class="message-label">human</div>'
                    f'{msg[1]}'
                    f'</div>'
                )
            elif msg[0] == 'text-assistant':
                parts.append(
                    f'<div class="message assistant">'
                    f'<div class="message-label">claude</div>'
                    f'{msg[1]}'
                    f'</div>'
                )
            elif msg[0] == 'work':
                summary_text = msg[1]
                inner_html = msg[2]
                desc_html = msg[3] if len(msg) > 3 and msg[3] else ''
                desc_block = f'<div class="work-desc">{desc_html}</div>' if desc_html else ''
                parts.append(
                    f'<details class="work-group">'
                    f'<summary><span class="work-icon">&#9881;</span> {summary_text}</summary>'
                    f'<div class="work-group-inner">{desc_block}{inner_html}</div>'
                    f'</details>'
                )

    # add JS to highlight active session in sidebar while scrolling
    parts.append("""
<script>
(function() {
    var links = document.querySelectorAll('.sidebar a[data-session]');
    var headers = document.querySelectorAll('.session-header');
    if (!links.length || !headers.length) return;
    var ticking = false;
    window.addEventListener('scroll', function() {
        if (!ticking) {
            ticking = true;
            requestAnimationFrame(function() {
                var scrollY = window.scrollY + 80;
                var active = 0;
                for (var i = 0; i < headers.length; i++) {
                    if (headers[i].offsetTop <= scrollY) active = i;
                }
                for (var j = 0; j < links.length; j++) {
                    links[j].classList.toggle('active', j === active);
                }
                ticking = false;
            });
        }
    });
    // initial highlight
    if (links[0]) links[0].classList.add('active');
})();
</script>
""")
    parts.append('</div></div></body></html>')
    return '\n'.join(parts)


def main():
    parser = argparse.ArgumentParser(description='Format Claude Code logs for a blog post')
    parser.add_argument('files', nargs='+', help='JSONL log files (will be sorted chronologically)')
    parser.add_argument('-o', '--output', default='conversation.html', help='Output HTML file')
    args = parser.parse_args()

    # sort files by first timestamp
    file_times = []
    for f in args.files:
        first_ts = ''
        with open(f) as fh:
            for line in fh:
                obj = json.loads(line)
                if 'timestamp' in obj:
                    first_ts = obj['timestamp']
                    break
        file_times.append((first_ts, f))
    file_times.sort()

    sessions = []
    for i, (_, filepath) in enumerate(file_times):
        title = SESSION_TITLES[i] if i < len(SESSION_TITLES) else f'session {i+1}'
        messages = process_session(filepath, i)
        if messages:
            sessions.append((title, messages))

    html_doc = generate_html(sessions)

    with open(args.output, 'w') as f:
        f.write(html_doc)

    # stats
    text_msgs = sum(1 for _, msgs in sessions for m in msgs if m[0].startswith('text-'))
    work_groups = sum(1 for _, msgs in sessions for m in msgs if m[0] == 'work')
    print(f'wrote {args.output} ({len(sessions)} sessions, {text_msgs} conversation messages, {work_groups} work groups, {os.path.getsize(args.output)//1024}KB)')


if __name__ == '__main__':
    main()
