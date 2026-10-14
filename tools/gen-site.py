#!/usr/bin/env python3
"""Generates the public website (https://accerberus.github.io) from docs/.

Everything on the site comes from the repository, so the site can never say
something the docs do not:

  VERSION                  the version in development
  docs/CHANGELOG.md        every release, its date and what it changed
  docs/TO_FINISH.md        which phases are done and which comes next
  docs/ROADMAP.md          each coming phase's items and "done when"
  docs/FEATURES.md         what exists today (the home page's cards)
  docs/BENCH.md            the newest measured numbers
  docs/BUGS.md             the open bugs
  docs/SYSCALLS.md         the system-call count
  tests/integration/       the integration-test count
  docs/screenshots/        the pictures
  docs/*.md                rendered one page each under /docs/

usage: python3 tools/gen-site.py [site-dir]     (default ../ACCerberus.github.io)

Standard library only (SPEC §0 rule 14). Run by `make site`; `make site-push`
also commits and pushes the result (tools/push-site.sh).
"""
import datetime
import html
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SITE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (ROOT.parent / "ACCerberus.github.io")
BASE_URL = "https://accerberus.github.io"
REPO_URL = "https://github.com/ArsenalRX/CerberusOS"
TODAY = datetime.date.today().isoformat()

# The release that completed each phase before the changelog named phases
# in its text (0.0.1 and 0.3.0 predate the per-phase headings).
EARLY_PHASE_RELEASE = {0: "0.3.0", 1: "0.3.0", 2: "0.3.0", 3: "0.3.0"}

# One line per document for the docs index; a file not listed here gets its
# own "What this file is for" paragraph instead.
DOC_BLURBS = {
    "ABOUT": "A plain-language tour of Cerberus and the Spec language, for anyone.",
    "STATUS": "What is true right now: the version, what is verified, what is half-done.",
    "ROADMAP": "Everything still to add, fix and speed up, phase by phase, written for a person.",
    "FEATURES": "The inventory of what exists today and how each thing is used.",
    "CHANGELOG": "What changed in every version, newest first.",
    "BUGS": "The bug ledger: every bug found, its cause, its fix and what guards against it.",
    "BENCH": "Measured performance over time, against the written budgets.",
    "DECISIONS": "Why the system is the way it is: every design decision, dated, with the alternatives rejected.",
    "SPEC": "The full specification: the contract the system is built to.",
    "TO_FINISH": "The working checklist: what is done, what is next, what remains.",
    "SYSCALLS": "The system-call reference, generated from the kernel's own table.",
    "INSTALLER": "Research notes for the installer, USB media and dual boot (phase 18).",
}
DOC_ORDER = ["ABOUT", "FEATURES", "STATUS", "ROADMAP", "CHANGELOG", "BUGS", "BENCH",
             "SYSCALLS", "DECISIONS", "SPEC", "TO_FINISH", "INSTALLER"]


def read(name):
    return (DOCS / name).read_text(encoding="utf-8")


def slugify(text):
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return text or "section"


# --------------------------------------------------------------------------
# Markdown: the subset the docs use (headings, paragraphs, nested lists with
# check boxes, tables, fenced code, block quotes, rules; inline code, bold,
# italic, links, images). Written here rather than imported (no dependencies).

def md_inline(text, in_code=False):
    codes = []

    def keep(m):
        codes.append(m.group(1))
        return "\x00%d\x00" % (len(codes) - 1)

    text = re.sub(r"`([^`\n]+)`", keep, text)
    text = html.escape(text, quote=False)
    text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)",
                  lambda m: '<img src="%s" alt="%s" loading="lazy" />' % (rewrite_link(m.group(2)), m.group(1)), text)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
                  lambda m: '<a href="%s">%s</a>' % (rewrite_link(m.group(2)), m.group(1)), text)
    text = re.sub(r"(?<![\"'=>])(https?://[^\s<)]+)", r'<a href="\1">\1</a>', text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![\w*])\*(?!\s)([^*\n]+?)(?<!\s)\*(?![\w*])", r"<em>\1</em>", text)
    text = text.replace("&amp;middot;", "&middot;")

    def restore(m):
        return "<code>%s</code>" % html.escape(codes[int(m.group(1))], quote=False)

    return re.sub(r"\x00(\d+)\x00", restore, text)


def rewrite_link(url):
    """docs/X.md → /docs/x/, screenshots → /img/screenshots/, the rest as is."""
    m = re.match(r"^(?:\.\./)?(?:docs/)?([A-Z_]+)\.md(#.*)?$", url)
    if m:
        return "/docs/%s/%s" % (m.group(1).lower(), m.group(2) or "")
    m = re.match(r"^(?:\.\./)?(?:docs/)?screenshots/([^/]+)$", url)
    if m:
        return "/img/screenshots/" + m.group(1)
    return url


class Md:
    def __init__(self, text):
        self.lines = text.split("\n")
        self.out = []
        self.headings = []  # (level, text, id)

    def render(self):
        i, n, lines = 0, len(self.lines), self.lines
        para = []

        def flush():
            if para:
                self.out.append("<p>%s</p>" % md_inline(" ".join(s.strip() for s in para)))
                para.clear()

        while i < n:
            line = lines[i]
            stripped = line.strip()
            if not stripped:
                flush(); i += 1; continue
            if stripped.startswith("```"):
                flush()
                j = i + 1
                while j < n and not lines[j].strip().startswith("```"):
                    j += 1
                code = "\n".join(lines[i + 1:j])
                self.out.append("<pre><code>%s</code></pre>" % html.escape(code, quote=False))
                i = j + 1; continue
            m = re.match(r"^(#{1,6})\s+(.*?)\s*#*$", stripped)
            if m:
                flush()
                level, text = len(m.group(1)), m.group(2)
                hid = slugify(text)
                base, k = hid, 2
                while any(h[2] == hid for h in self.headings):
                    hid = "%s-%d" % (base, k); k += 1
                self.headings.append((level, text, hid))
                self.out.append('<h%d id="%s">%s</h%d>' % (level, hid, md_inline(text), level))
                i += 1; continue
            if re.match(r"^(-{3,}|\*{3,}|_{3,})$", stripped):
                flush(); self.out.append("<hr />"); i += 1; continue
            if stripped.startswith(">"):
                flush()
                j = i
                quote = []
                while j < n and lines[j].strip().startswith(">"):
                    quote.append(re.sub(r"^\s*>\s?", "", lines[j])); j += 1
                inner = Md("\n".join(quote)).render()
                self.out.append("<blockquote>%s</blockquote>" % inner)
                i = j; continue
            if stripped.startswith("|") and i + 1 < n and re.match(r"^\s*\|?\s*:?-{2,}", lines[i + 1]):
                flush()
                j = i
                rows = []
                while j < n and lines[j].strip().startswith("|"):
                    rows.append(lines[j].strip()); j += 1
                self.out.append(self.table(rows))
                i = j; continue
            if re.match(r"^\s*([-*+]|\d+[.)])\s+", line):
                flush()
                j = i
                block = []
                while j < n:
                    l = lines[j]
                    if not l.strip():
                        # a blank line ends the list unless the next non-blank line is indented
                        k = j + 1
                        while k < n and not lines[k].strip():
                            k += 1
                        if k < n and re.match(r"^\s+\S", lines[k]):
                            j += 1; continue
                        break
                    if re.match(r"^\s*([-*+]|\d+[.)])\s+", l) or re.match(r"^\s+\S", l):
                        block.append(l); j += 1; continue
                    break
                self.out.append(self.list(block))
                i = j; continue
            para.append(line)
            i += 1
        flush()
        return "\n".join(self.out)

    def table(self, rows):
        def cells(r):
            r = r.strip()
            if r.startswith("|"): r = r[1:]
            if r.endswith("|"): r = r[:-1]
            return [c.strip() for c in re.split(r"(?<!\\)\|", r)]
        head = cells(rows[0])
        body = [cells(r) for r in rows[2:]]
        h = "".join("<th>%s</th>" % md_inline(c) for c in head)
        b = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % md_inline(c) for c in r) for r in body)
        return '<div class="table"><table><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>' % (h, b)

    def list(self, block):
        # items: (indent, ordered, text-lines)
        items = []
        for l in block:
            m = re.match(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$", l)
            if m:
                items.append([len(m.group(1).expandtabs(4)), m.group(2)[0].isdigit(), [m.group(3)]])
            elif items:
                items[-1][2].append(l.strip())
        return self.nest(items, 0, len(items), items[0][0] if items else 0)[0]

    def nest(self, items, start, end, indent):
        ordered = items[start][1]
        out = ["<ol>" if ordered else "<ul>"]
        i = start
        while i < end:
            ind, _, text = items[i]
            j = i + 1
            while j < end and items[j][0] > ind:
                j += 1
            body = self.item_text(text)
            child = ""
            if j > i + 1:
                child, _ = self.nest(items, i + 1, j, items[i + 1][0])
            out.append("<li>%s%s</li>" % (body, child))
            i = j
        out.append("</ol>" if ordered else "</ul>")
        return "\n".join(out), i

    def item_text(self, lines):
        text = " ".join(l for l in lines if l).strip()
        m = re.match(r"^\[( |x|X)\]\s+(.*)$", text)
        if m:
            done = m.group(1).lower() == "x"
            return '<span class="box %s" aria-label="%s"></span>%s' % (
                "done" if done else "todo", "done" if done else "to do", md_inline(m.group(2)))
        return md_inline(text)


# --------------------------------------------------------------------------
# Readers for the facts the site shows.

def git_tags():
    import subprocess
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "tag"], capture_output=True, text=True, timeout=20)
        return set(out.stdout.split())
    except Exception:  # no git: every changelog block counts as released
        return None


def parse_changelog():
    text = read("CHANGELOG.md")
    releases, unreleased = [], None
    cur = None
    for line in text.split("\n"):
        m = re.match(r"^## (\S+)(?: — (\d{4}-\d{2}-\d{2}))?(.*)$", line)
        if m:
            cur = {"v": m.group(1), "date": m.group(2), "security": "security" in m.group(3).lower(),
                   "intro": [], "groups": {}, "group": None}
            if m.group(1).lower() == "unreleased":
                unreleased = cur
            else:
                releases.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"^### (\w+)", line)
        if m:
            cur["group"] = m.group(1)
            cur["groups"][m.group(1)] = []
            continue
        if cur["group"]:
            if re.match(r"^- ", line):
                cur["groups"][cur["group"]].append(line[2:].strip())
            elif line.startswith("  ") and cur["groups"][cur["group"]]:
                cur["groups"][cur["group"]][-1] += " " + line.strip()
        elif line.strip() and not line.startswith("#"):
            cur["intro"].append(line.strip())
    tags = git_tags()
    for r in releases + ([unreleased] if unreleased else []):
        # a changelog block is only a release once its tag exists; until then
        # it is "built, not yet released" (the site must not claim otherwise)
        r["tagged"] = tags is None or ("v" + r["v"]) in tags
        r["intro"] = " ".join(r["intro"])
        r["groups"] = {k: v for k, v in r["groups"].items() if v}
        m = re.search(r"phases? ([\d–\-, ]+\d|\d+)", r["intro"], re.I)
        r["phase"] = ("Phase%s %s" % ("s" if re.search(r"[–\-,]", m.group(1)) else "", m.group(1).strip())) if m else ""
        if r["phase"] and not re.search(r"complete|done|is the", r["intro"], re.I):
            r["phase"] = ""
    return releases, unreleased


def parse_phases():
    """From TO_FINISH.md: the spec phases in order with done flag and short text."""
    text = read("TO_FINISH.md")
    sec = re.search(r"^## Spec phases remaining.*?$(.*?)^## ", text, re.S | re.M)
    body = sec.group(1) if sec else text
    phases = []
    for m in re.finditer(r"^- \[( |x)\] \*\*Phase (\d+[A-Z]?) — ([^*]+?)\.?\*\*(.*?)(?=^- \[|\Z)", body, re.S | re.M):
        rest = " ".join(s.strip() for s in m.group(4).split("\n")).strip()
        rest = re.split(r"\s\*\*\+v2:\*\*", rest)[0].strip()
        phases.append({"n": m.group(2), "title": m.group(3).strip(), "done": m.group(1) == "x",
                       "text": first_sentence(rest, 220)})
    done = []
    sec = re.search(r"^## Done \(committed\)(.*?)^## ", text, re.S | re.M)
    if sec:
        for m in re.finditer(r"^- \*\*Phase (\d+) — ([^.*]+)\.(?: Release ([0-9a-z.]+) \((\d{4}-\d{2}-\d{2})\)\.)?\*\*(.*?)(?=^- |\Z)",
                             sec.group(1), re.S | re.M):
            rest = " ".join(s.strip() for s in m.group(5).split("\n")).strip()
            done.append({"n": int(m.group(1)), "title": m.group(2).strip(), "v": m.group(3) or EARLY_PHASE_RELEASE.get(int(m.group(1)), ""),
                         "date": m.group(4) or "", "text": first_sentence(rest, 160)})
    done.sort(key=lambda d: d["n"])
    return phases, done


def parse_roadmap_phases():
    """From ROADMAP.md: each '## Phase N — title' section's intro, items and goal."""
    text = read("ROADMAP.md")
    out = {}
    for m in re.finditer(r"^## Phase (\d+[A-Z]?) — (.+?)$(.*?)(?=^## |\Z)", text, re.S | re.M):
        n, title, body = m.group(1), m.group(2).strip(), m.group(3)
        intro, items, goal, decision = [], [], "", ""
        mode = None
        for line in body.split("\n"):
            s = line.strip()
            if not s:
                mode = None; continue
            mm = re.match(r"^- \[( |x)\] (.*)$", s)
            if mm:
                items.append({"done": mm.group(1) == "x", "text": mm.group(2)}); mode = "item"; continue
            if mode == "item" and line.startswith("  "):
                items[-1]["text"] += " " + s; continue
            if s.startswith("**Done when:**"):
                goal = s[len("**Done when:**"):].strip(); mode = "goal"; continue
            if s.startswith("**Owner decision first:**"):
                decision = s[len("**Owner decision first:**"):].strip(); mode = "decision"; continue
            if re.match(r"^\*\*(Add|Security|Speed|Fix)\*\*$", s):
                mode = None; continue
            if mode == "goal":
                goal += " " + s; continue
            if mode == "decision":
                decision += " " + s; continue
            if not items and not goal:
                intro.append(s); mode = "intro"
        out[n] = {"n": n, "title": title, "intro": " ".join(intro), "items": items,
                  "goal": goal.strip(), "decision": decision.strip()}
    return out


def parse_bench():
    text = read("BENCH.md")
    blocks = re.split(r"^## ", text, flags=re.M)[1:]
    if not blocks:
        return {}, ""
    last = blocks[-1]
    title = last.split("\n", 1)[0].strip()
    metrics = {}
    for row in re.finditer(r"^\| ([^|]+?) \| ([^|]+?) \|", last, re.M):
        name, val = row.group(1).strip(), row.group(2).strip()
        if name.lower() in ("metric", "case", "what") or set(name) <= set("-"):
            continue
        num = re.search(r"([\d,]+(?:\.\d+)?)\s*(ns|µs|ms|%|MiB/s|MB/s)", val)
        if num and name not in metrics:
            metrics[name] = (num.group(1), num.group(2))
    return metrics, title


def metric(metrics, *keys):
    for k in keys:
        for name, v in metrics.items():
            if k.lower() in name.lower():
                return v
    return None


def parse_features():
    text = read("FEATURES.md")
    areas = []
    for m in re.finditer(r"^## (.+?)$(.*?)(?=^## |\Z)", text, re.S | re.M):
        title, body = m.group(1), m.group(2)
        title_short = re.sub(r"\s*\(.*?\)\s*", "", title).strip()
        bullets = re.findall(r"^- (.*?)(?=^- |^#|\Z)", body, re.S | re.M)
        bullets = [" ".join(s.strip() for s in b.split("\n")).strip() for b in bullets]
        areas.append({"title": title_short, "id": slugify(title), "bullets": bullets,
                      "text": body.strip()})
    return areas


def parse_bugs():
    text = read("BUGS.md")
    sec = re.search(r"^## Open(.*?)^## Fixed", text, re.S | re.M)
    body = sec.group(1) if sec else ""
    return re.findall(r"^### (B-\d+) — (.+)$", body, re.M)


def count_syscalls():
    return len(re.findall(r"^\| \d+ \|", read("SYSCALLS.md"), re.M))


def count_tests():
    return len(list((ROOT / "tests" / "integration").glob("*.expect")))


def first_sentence(text, limit):
    text = re.sub(r"\s+", " ", text).strip()
    m = re.match(r"^(.*?[.!?])(\s|$)", text)
    s = m.group(1) if m and len(m.group(1)) <= limit else text
    if len(s) > limit:
        s = s[:limit].rsplit(" ", 1)[0]
        if s.count("`") % 2:  # never cut a code span in half
            s = s[:s.rfind("`")].rstrip(" ,;:")
        s += "…"
    return s


def plain(md):
    """Markdown → plain text for short blurbs."""
    t = re.sub(r"`([^`]+)`", r"\1", md)
    t = re.sub(r"\*\*(.+?)\*\*", r"\1", t)
    t = re.sub(r"\*(.+?)\*", r"\1", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)
    return t


# --------------------------------------------------------------------------
# HTML

CSS = r"""
*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }
:root {
  --bg: #06070f; --surface: rgba(255,255,255,0.03); --surface-hover: rgba(255,255,255,0.05);
  --border: rgba(255,255,255,0.08); --border-bright: rgba(80,170,255,0.4);
  --accent: #38b6ff; --accent-strong: #0a84ff; --violet: #8b5cf6;
  --grad: linear-gradient(95deg, #3ad4ff, #3b8bff 45%, #9b6bff);
  --green: #35d07f; --amber: #ffb65a; --red: #ff6b7d;
  --text: #aab1c5; --text-bright: #f1f3f9; --text-dim: #6a7189;
  --font: 'Inter', -apple-system, 'Segoe UI', sans-serif; --font-mono: 'JetBrains Mono', Consolas, monospace;
  --max: 1120px;
}
html { scroll-behavior: smooth; scroll-padding-top: 84px; }
body { background: var(--bg); color: var(--text); font-family: var(--font); font-size: 16px; line-height: 1.6; -webkit-font-smoothing: antialiased; overflow-x: hidden; }
body.narrow { --max: 920px; }
.aurora { position: fixed; inset: 0; z-index: -2; overflow: hidden; pointer-events: none; }
.aurora i { position: absolute; border-radius: 50%; will-change: transform; animation: drift 26s ease-in-out infinite alternate; }
.aurora i:nth-child(1) { width: 1100px; height: 800px; right: -300px; top: -320px; background: radial-gradient(closest-side, rgba(20,100,255,0.55), transparent); }
.aurora i:nth-child(2) { width: 900px; height: 800px; left: -420px; top: 30%; background: radial-gradient(closest-side, rgba(120,60,240,0.38), transparent); animation-duration: 32s; animation-direction: alternate-reverse; }
.aurora i:nth-child(3) { width: 900px; height: 700px; right: -200px; bottom: -380px; background: radial-gradient(closest-side, rgba(0,150,255,0.30), transparent); animation-duration: 38s; }
@keyframes drift { to { transform: translate3d(-90px, 70px, 0) scale(1.12); } }
.progress { position: fixed; top: 0; left: 0; right: 0; height: 2px; z-index: 20; background: var(--grad); transform-origin: 0 50%; transform: scaleX(0); }
body.home::after { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 1000px; z-index: -1; pointer-events: none;
  background-image: linear-gradient(rgba(120,180,255,0.06) 1px, transparent 1px), linear-gradient(90deg, rgba(120,180,255,0.06) 1px, transparent 1px);
  background-size: 56px 56px; -webkit-mask-image: radial-gradient(ellipse 70% 60% at 50% 20%, #000, transparent 75%); mask-image: radial-gradient(ellipse 70% 60% at 50% 20%, #000, transparent 75%); }
a { color: inherit; text-decoration: none; }
:focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; border-radius: 6px; }
.wrap { max-width: var(--max); margin: 0 auto; padding: 0 24px; }
code { font-family: var(--font-mono); font-size: 0.86em; padding: 1px 6px; border-radius: 5px; background: rgba(255,255,255,0.06); color: #c9d2e8; }

.nav { position: sticky; top: 14px; z-index: 10; padding: 0 16px; margin-top: 14px; }
.nav .wrap { max-width: 1120px; display: flex; align-items: center; justify-content: space-between; height: 56px; padding: 0 12px 0 22px; border-radius: 16px;
  border: 1px solid var(--border); background: rgba(9,11,24,0.62); backdrop-filter: blur(18px) saturate(160%); -webkit-backdrop-filter: blur(18px) saturate(160%);
  box-shadow: 0 10px 40px rgba(0,0,0,0.35), 0 1px 0 rgba(255,255,255,0.05) inset; }
.wordmark { font-weight: 700; font-size: 20px; letter-spacing: -0.02em; color: var(--text-bright); display: inline-flex; align-items: baseline; gap: 5px; }
.wordmark i { width: 8px; height: 8px; border-radius: 50%; background: var(--accent); box-shadow: 0 0 16px var(--accent), 0 0 4px #fff; }
.nav-links { display: flex; align-items: center; gap: 26px; font-size: 14px; padding-right: 10px; }
.nav-links a:not(.btn) { color: var(--text); transition: color .15s; }
.nav-links a:not(.btn):hover, .nav-links a.on { color: var(--text-bright); }
.menu-btn { display: none; place-items: center; width: 40px; height: 40px; border-radius: 11px; border: 1px solid var(--border); background: var(--surface); cursor: pointer; }
.menu-btn span, .menu-btn span::before { display: block; width: 16px; height: 2px; border-radius: 2px; background: var(--text-bright); transition: transform .2s; }
.menu-btn span { position: relative; transform: translateY(-3px); }
.menu-btn span::before { content: ''; position: absolute; top: 6px; }
.nav.open .menu-btn span { transform: rotate(45deg); }
.nav.open .menu-btn span::before { top: 0; transform: rotate(-90deg); }

.btn { display: inline-flex; align-items: center; gap: 8px; padding: 11px 20px; border-radius: 12px; font-size: 14px; font-weight: 600; cursor: pointer;
  border: 1px solid var(--border); color: var(--text-bright); background: var(--surface); transition: background .15s, border-color .15s, transform .15s; font-family: var(--font); }
.btn:hover { background: var(--surface-hover); border-color: var(--border-bright); transform: translateY(-1px); }
.btn.primary { position: relative; overflow: hidden; background: linear-gradient(180deg, #2aa0ff, #0a6cf0); border-color: rgba(255,255,255,0.18); box-shadow: 0 10px 40px rgba(10,132,255,0.5), 0 1px 0 rgba(255,255,255,0.35) inset; }
.btn.primary::after { content: ''; position: absolute; top: 0; bottom: 0; left: -60%; width: 40%; transform: skewX(-20deg); background: linear-gradient(90deg, transparent, rgba(255,255,255,0.35), transparent); transition: left .6s ease; }
.btn.primary:hover::after { left: 130%; }
.btn.small { padding: 7px 14px; font-size: 13.5px; }

.hero { padding: 104px 0 40px; text-align: center; }
.head { padding: 80px 0 8px; }
.pill { display: inline-flex; flex-wrap: wrap; justify-content: center; align-items: center; gap: 10px; padding: 6px 14px; border-radius: 999px;
  border: 1px solid var(--border); background: var(--surface); font-family: var(--font-mono); font-size: 12.5px; color: var(--text); }
.pill b { width: 6px; height: 6px; border-radius: 50%; background: var(--green); box-shadow: 0 0 10px var(--green); }
h1 { margin: 28px auto 20px; max-width: 880px; font-size: clamp(42px, 7.4vw, 84px); line-height: 1.02; letter-spacing: -0.045em; font-weight: 700; color: var(--text-bright); }
.head h1 { margin: 16px 0 18px; font-size: clamp(36px, 6vw, 60px); max-width: none; }
h1 span { background: var(--grad); background-size: 200% 100%; -webkit-background-clip: text; background-clip: text; color: transparent; animation: sheen 8s ease-in-out infinite alternate; filter: drop-shadow(0 0 28px rgba(60,140,255,0.35)); }
@keyframes sheen { to { background-position: 100% 0; } }
.lead { max-width: 620px; margin: 0 auto 36px; font-size: 18px; }
.head .lead { margin: 0; }
.cta, .jump { display: flex; gap: 12px; justify-content: center; flex-wrap: wrap; }
.jump { justify-content: flex-start; margin-top: 30px; gap: 10px; }

.shot { position: relative; border-radius: 16px; overflow: hidden; border: 1px solid var(--border-bright); background: #0b0d18; box-shadow: 0 40px 120px rgba(30,70,180,0.35), 0 0 0 1px rgba(255,255,255,0.03) inset; }
.shot img { display: block; width: 100%; height: auto; cursor: zoom-in; transition: transform .5s ease; }
.shot:hover img { transform: scale(1.015); }
.shot figcaption { padding: 10px 14px; font-size: 13px; color: var(--text-dim); border-top: 1px solid var(--border); }
@property --spin { syntax: '<angle>'; initial-value: 0deg; inherits: false; }
.hero-shot { position: relative; margin: 80px auto 0; max-width: 1040px; overflow: visible; border: 0; background: none; box-shadow: none; perspective: 1800px; }
.hero-shot::before { content: ''; position: absolute; inset: -12% -8% -6%; z-index: -1; background: radial-gradient(closest-side, rgba(20,120,255,0.55), transparent 70%); filter: blur(30px); }
.tilt { position: relative; padding: 1.5px; border-radius: 16px; transform: rotateX(var(--tilt, 9deg)); transform-origin: 50% 0; will-change: transform;
  background: conic-gradient(from var(--spin), rgba(120,180,255,0.25), #3ad4ff, #9b6bff, rgba(120,180,255,0.25) 40%, rgba(120,180,255,0.25));
  animation: spin 7s linear infinite; box-shadow: 0 60px 140px rgba(10,60,200,0.55), 0 0 60px rgba(40,140,255,0.25); }
.tilt img { border-radius: 14.5px; }
@keyframes spin { to { --spin: 360deg; } }

.stats { display: grid; grid-template-columns: repeat(4, 1fr); margin-top: 80px; position: relative; border: 1px solid var(--border); border-radius: 20px; background: linear-gradient(180deg, rgba(255,255,255,0.05), rgba(255,255,255,0.015)); backdrop-filter: blur(10px); }
.stats::before { content: ''; position: absolute; top: -1px; left: 12%; right: 12%; height: 1px; background: linear-gradient(90deg, transparent, var(--accent), var(--violet), transparent); }
.stat { padding: 28px 24px; text-align: center; border-left: 1px solid var(--border); }
.stat:first-child { border-left: 0; }
.stat strong { display: block; font-size: 40px; font-variant-numeric: tabular-nums; letter-spacing: -0.03em; color: var(--text-bright); font-weight: 600; }
.stat strong small { font-size: 16px; color: var(--accent); margin-left: 3px; font-weight: 500; }
.stat > span { font-size: 13.5px; color: var(--text-dim); }

section { padding: 112px 0 0; }
.eyebrow { display: inline-flex; align-items: center; gap: 12px; font-family: var(--font-mono); font-size: 12.5px; letter-spacing: 0.16em; text-transform: uppercase; color: var(--accent); }
.eyebrow::before { content: ''; width: 28px; height: 1px; background: linear-gradient(90deg, transparent, var(--accent)); }
h2 { margin: 14px 0 16px; font-size: clamp(28px, 4vw, 42px); line-height: 1.12; letter-spacing: -0.03em; color: var(--text-bright); font-weight: 600; max-width: 720px; }
.sub { max-width: 640px; }
.grid { display: grid; gap: 16px; margin-top: 44px; }
.g3 { grid-template-columns: repeat(3, 1fr); } .g2 { grid-template-columns: repeat(2, 1fr); } .g4 { grid-template-columns: repeat(4, 1fr); }
.card { position: relative; overflow: hidden; padding: 26px; border-radius: 18px; border: 1px solid var(--border); background: linear-gradient(180deg, rgba(255,255,255,0.05), rgba(255,255,255,0.015)); backdrop-filter: blur(6px); transition: border-color .25s, transform .25s, box-shadow .25s; }
.card::before { content: ''; position: absolute; inset: 0; pointer-events: none; opacity: 0; transition: opacity .3s; background: radial-gradient(360px circle at var(--mx, 50%) var(--my, 0%), rgba(56,182,255,0.16), transparent 60%); }
.card:hover { border-color: var(--border-bright); transform: translateY(-3px); box-shadow: 0 20px 50px rgba(5,40,140,0.35); }
.card:hover::before { opacity: 1; }
.card > * { position: relative; }
.card .num { font-family: var(--font-mono); font-size: 12.5px; color: var(--accent); }
.card .ico { display: grid; place-items: center; width: 38px; height: 38px; margin-bottom: 14px; border-radius: 11px; color: var(--accent); border: 1px solid rgba(56,182,255,0.3); background: rgba(56,182,255,0.08); box-shadow: 0 0 24px rgba(56,182,255,0.18) inset; }
.card .ico svg { width: 18px; height: 18px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; }
.card .ico + h3 { margin-top: 0; font-size: 16.5px; }
.card h3 { margin: 10px 0 8px; font-size: 18px; color: var(--text-bright); font-weight: 600; letter-spacing: -0.01em; }
.card p { font-size: 14.5px; }
.card.slim { padding: 20px 22px; } .card.slim h3 { margin-top: 0; font-size: 15.5px; } .card.slim p { font-size: 14px; }
.card .more { display: inline-block; margin-top: 10px; font-size: 13px; color: var(--accent); }
.card.doc h3 { margin-top: 0; }
.card.doc .path { font-family: var(--font-mono); font-size: 12px; color: var(--text-dim); margin-bottom: 8px; }

.vow { margin-top: 44px; padding: 34px 36px; border-radius: 22px; border: 1px solid var(--border-bright); display: grid; grid-template-columns: auto 1fr; gap: 28px; align-items: center;
  background: radial-gradient(500px 200px at 0 0, rgba(20,120,255,0.3), transparent 70%), radial-gradient(400px 200px at 100% 100%, rgba(139,92,246,0.22), transparent 70%), rgba(255,255,255,0.02); }
.vow .zero { font-size: 84px; line-height: 1; font-weight: 700; letter-spacing: -0.05em; background: var(--grad); -webkit-background-clip: text; background-clip: text; color: transparent; }
.vow h3 { font-size: 22px; color: var(--text-bright); font-weight: 600; letter-spacing: -0.02em; margin-bottom: 6px; }
.soon, .work { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 16px; }
.soon div, .work div { display: grid; grid-template-columns: 84px 1fr; gap: 14px; align-items: baseline; padding: 16px 20px; border-radius: 14px; border: 1px dashed var(--border); font-size: 14.5px; }
.work div { grid-template-columns: 1fr; }
.soon code, .work code { font-family: var(--font-mono); font-size: 12px; color: var(--accent); background: none; padding: 0; }
.soon b, .work b { color: var(--text-bright); font-weight: 600; }
.minor { margin-top: 56px; font-family: var(--font-mono); font-size: 12.5px; letter-spacing: 0.14em; text-transform: uppercase; color: var(--text-dim); }

.track { display: grid; grid-template-columns: repeat(20, 1fr); gap: 5px; margin-top: 44px; }
.track i { height: 8px; border-radius: 4px; background: rgba(255,255,255,0.08); }
.track i.done { background: var(--grad); box-shadow: 0 0 14px rgba(56,182,255,0.55); }
.track i.next { background: rgba(90,162,255,0.35); animation: pulse 1.8s ease-in-out infinite; }
@keyframes pulse { 50% { opacity: .35; } }
.track-label { display: flex; justify-content: space-between; margin-top: 12px; font-family: var(--font-mono); font-size: 12.5px; color: var(--text-dim); }
.phases { margin-top: 36px; border-top: 1px solid var(--border); }
.phase-row { display: grid; grid-template-columns: 90px 240px 1fr; gap: 20px; padding: 18px 0; border-bottom: 1px solid var(--border); font-size: 14.5px; align-items: baseline; }
.phase-row code { font-family: var(--font-mono); font-size: 12.5px; color: var(--text-dim); background: none; padding: 0; }
.phase-row b { color: var(--text-bright); font-weight: 600; }
.phase-row.up code { color: var(--accent); }

.split { display: grid; grid-template-columns: 1fr 1fr; gap: 56px; align-items: center; }
.split ul { list-style: none; margin-top: 24px; display: grid; gap: 12px; font-size: 15px; }
.split li { padding-left: 22px; position: relative; }
.split li::before { content: ''; position: absolute; left: 0; top: 10px; width: 6px; height: 6px; border-radius: 50%; background: var(--accent); }
.split li b { color: var(--text-bright); font-weight: 600; }
.code { border-radius: 16px; border: 1px solid var(--border-bright); background: rgba(6,9,20,0.78); backdrop-filter: blur(10px); overflow: hidden; box-shadow: 0 30px 90px rgba(5,50,170,0.35); }
.code header { display: flex; align-items: center; justify-content: space-between; padding: 10px 16px; border-bottom: 1px solid var(--border); font-family: var(--font-mono); font-size: 12.5px; color: var(--text-dim); }
.code pre { padding: 20px; overflow-x: auto; font-family: var(--font-mono); font-size: 13.5px; line-height: 1.75; color: #c9d2e8; }
.code .k { color: var(--accent); } .code .t { color: #b79bff; }
.note { margin-top: 14px; font-size: 13px; color: var(--text-dim); }
.stack { display: grid; gap: 10px; }
.layer { display: grid; grid-template-columns: 120px 1fr auto; gap: 16px; align-items: center; padding: 18px 20px; border-radius: 14px; border: 1px solid var(--border); background: var(--surface); font-size: 14px; }
.layer b { color: var(--text-bright); font-weight: 600; font-size: 15px; }
.layer em { font-style: normal; font-family: var(--font-mono); font-size: 11.5px; padding: 4px 10px; border-radius: 999px; border: 1px solid var(--border); color: var(--text-dim); white-space: nowrap; }
.layer.live { border-color: var(--border-bright); background: linear-gradient(90deg, rgba(56,182,255,0.16), rgba(139,92,246,0.10)); box-shadow: 0 0 60px rgba(20,110,255,0.3); }
.layer.live em { color: var(--green); border-color: rgba(53,208,127,0.35); }
.layer.hw { border-style: dashed; background: none; }
.metric strong { display: block; font-size: 36px; letter-spacing: -0.03em; font-weight: 600; background: linear-gradient(100deg, #fff, var(--accent)); -webkit-background-clip: text; background-clip: text; color: transparent; }
.metric strong small { font-size: 15px; margin-left: 4px; }
.metric p { margin-top: 6px; font-size: 13.5px; }
.keys { display: grid; gap: 12px; margin-top: 28px; font-size: 14.5px; }
.keys div { display: flex; align-items: center; gap: 14px; }
.keys span { min-width: 128px; display: inline-flex; gap: 5px; align-items: center; color: var(--text-dim); }
kbd { font-family: var(--font-mono); font-size: 12px; padding: 4px 9px; border-radius: 7px; color: var(--text-bright); background: rgba(255,255,255,0.06); border: 1px solid var(--border); border-bottom-width: 2px; }

.news { margin-top: 36px; display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
.news h4 { margin: 0 0 10px; font-family: var(--font-mono); font-size: 12px; letter-spacing: 0.12em; text-transform: uppercase; font-weight: 500; display: flex; align-items: center; gap: 8px; }
.news h4::before, .body h4::before { content: ''; width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
h4.security { color: var(--amber); } h4.added { color: var(--green); } h4.changed { color: var(--accent); } h4.performance { color: #b79bff; } h4.fixed { color: var(--red); } h4.removed { color: var(--text-dim); }
.news ul, .body ul { list-style: none; display: grid; gap: 8px; font-size: 14.5px; }
.news li, .body li { padding-left: 18px; position: relative; }
.news li::before, .body li::before { content: ''; position: absolute; left: 2px; top: 11px; width: 6px; height: 1px; background: var(--text-dim); }

.closing { margin-top: 128px; padding: 72px 32px; text-align: center; border-radius: 24px; border: 1px solid var(--border-bright); position: relative; overflow: hidden;
  background: radial-gradient(600px 300px at 50% 0, rgba(60,110,240,0.35), transparent 70%), radial-gradient(500px 300px at 20% 100%, rgba(122,92,255,0.25), transparent 70%), rgba(255,255,255,0.02); }
.closing h2 { margin: 0 auto 14px; } .closing p { max-width: 520px; margin: 0 auto 30px; }
.closing .wordmark { font-size: 28px; margin-bottom: 26px; } .closing .wordmark i { width: 11px; height: 11px; }
.lightbox { position: fixed; inset: 0; z-index: 50; display: none; place-items: center; padding: 24px; background: rgba(3,4,10,0.88); backdrop-filter: blur(8px); cursor: zoom-out; }
.lightbox.open { display: grid; }
.lightbox img { max-width: 100%; max-height: 100%; border-radius: 12px; border: 1px solid var(--border-bright); box-shadow: 0 40px 140px rgba(30,70,180,0.5); }
footer { margin-top: 96px; border-top: 1px solid var(--border); padding: 32px 0 48px; font-size: 13.5px; color: var(--text-dim); }
footer .wrap { max-width: 1120px; display: flex; justify-content: space-between; gap: 16px; flex-wrap: wrap; align-items: center; }
footer a:hover { color: var(--text-bright); } footer nav { display: flex; gap: 22px; flex-wrap: wrap; }
.reveal { opacity: 0; transform: translateY(16px); transition: opacity .6s ease, transform .6s ease; }
.reveal.in { opacity: 1; transform: none; }

/* roadmap page */
.ahead { margin-top: 44px; position: relative; padding-left: 34px; }
.ahead::before { content: ''; position: absolute; left: 7px; top: 8px; bottom: 8px; width: 1px; background: linear-gradient(var(--accent), rgba(122,92,255,0.5) 40%, var(--border)); }
.phase { position: relative; margin-bottom: 16px; padding: 24px 26px; border-radius: 18px; border: 1px solid var(--border); background: linear-gradient(180deg, rgba(255,255,255,0.045), rgba(255,255,255,0.015)); backdrop-filter: blur(6px); transition: border-color .2s, background .2s; }
.phase:hover { border-color: var(--border-bright); background: var(--surface-hover); }
.phase::before { content: ''; position: absolute; left: -32px; top: 30px; width: 11px; height: 11px; border-radius: 50%; background: var(--bg); border: 2px solid var(--text-dim); }
.phase.next { border-color: var(--border-bright); background: linear-gradient(120deg, rgba(56,182,255,0.12), rgba(139,92,246,0.08)); box-shadow: 0 0 70px rgba(20,110,255,0.2); }
.phase.next::before { border-color: var(--accent); background: var(--accent); box-shadow: 0 0 14px var(--accent); }
.phase header { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }
.phase .n { font-family: var(--font-mono); font-size: 12.5px; color: var(--accent); }
.phase h3 { font-size: 19px; color: var(--text-bright); font-weight: 600; letter-spacing: -0.01em; }
.phase .tag { margin-left: auto; font-family: var(--font-mono); font-size: 11.5px; padding: 3px 10px; border-radius: 999px; border: 1px solid var(--border); color: var(--text-dim); }
.phase.next .tag { color: var(--green); border-color: rgba(53,208,127,0.35); }
.phase > p { margin-top: 8px; font-size: 15px; }
.phase ul { list-style: none; margin-top: 16px; display: grid; grid-template-columns: 1fr 1fr; gap: 9px 28px; font-size: 14.5px; }
.phase li { padding-left: 18px; position: relative; }
.phase li::before { content: ''; position: absolute; left: 0; top: 10px; width: 5px; height: 5px; border-radius: 50%; background: var(--accent); }
.phase li.done::before { background: var(--green); }
.phase li b { color: var(--text-bright); font-weight: 600; }
.phase .goal { margin-top: 18px; padding-top: 14px; border-top: 1px solid var(--border); font-size: 13.5px; color: var(--text-dim); }
.phase .goal b { color: var(--text); font-weight: 600; }
.finished { margin-top: 36px; border-top: 1px solid var(--border); }
.finished a { display: grid; grid-template-columns: 44px 1fr auto; gap: 16px; align-items: baseline; padding: 15px 8px; border-bottom: 1px solid var(--border); font-size: 14.5px; transition: background .15s; }
.finished a:hover { background: var(--surface-hover); }
.finished a > code { background: none; padding: 0; font-size: 13px; color: var(--green); }
.finished b { color: var(--text-bright); font-weight: 600; }
.finished em { font-style: normal; font-family: var(--font-mono); font-size: 11.5px; padding: 3px 10px; border-radius: 999px; border: 1px solid var(--border); color: var(--text-dim); }
.bar { display: flex; justify-content: space-between; align-items: end; gap: 20px; flex-wrap: wrap; }
.releases { margin-top: 36px; display: grid; gap: 10px; }
details { border-radius: 14px; border: 1px solid var(--border); background: var(--surface); transition: border-color .2s, background .2s; }
details:hover { border-color: var(--border-bright); }
details[open] { border-color: var(--border-bright); background: rgba(255,255,255,0.04); }
summary { list-style: none; cursor: pointer; display: grid; grid-template-columns: 96px 1fr auto 18px; gap: 16px; align-items: center; padding: 18px 22px; }
summary::-webkit-details-marker { display: none; }
summary .v { font-family: var(--font-mono); font-size: 15px; font-weight: 500; color: var(--text-bright); }
summary .t { font-size: 15px; color: var(--text); }
summary .t small { display: block; font-family: var(--font-mono); font-size: 12px; color: var(--text-dim); }
summary .chips { display: flex; gap: 6px; }
.chip { font-family: var(--font-mono); font-size: 11px; padding: 3px 9px; border-radius: 999px; border: 1px solid var(--border); color: var(--text-dim); white-space: nowrap; }
.chip.latest { color: var(--green); border-color: rgba(53,208,127,0.35); }
.chip.sec { color: var(--amber); border-color: rgba(255,182,90,0.3); }
.chip.ph { color: var(--accent); border-color: rgba(90,162,255,0.3); }
.chip.dev { color: #b79bff; border-color: rgba(183,155,255,0.35); }
summary .arrow { width: 9px; height: 9px; border-right: 2px solid var(--text-dim); border-bottom: 2px solid var(--text-dim); transform: rotate(45deg); transition: transform .2s; justify-self: center; margin-top: -4px; }
details[open] summary .arrow { transform: rotate(-135deg); margin-top: 4px; }
.body { padding: 4px 22px 22px; border-top: 1px solid var(--border); }
.body > p { margin-top: 16px; font-size: 14.5px; }
.body h4 { margin: 20px 0 10px; font-family: var(--font-mono); font-size: 12px; letter-spacing: 0.12em; text-transform: uppercase; font-weight: 500; display: flex; align-items: center; gap: 8px; }

/* docs pages */
.prose { margin-top: 36px; max-width: 820px; }
.prose h1 { font-size: clamp(32px, 5vw, 48px); margin: 0 0 16px; text-align: left; }
.prose h2 { margin: 48px 0 12px; font-size: clamp(24px, 3vw, 32px); max-width: none; padding-top: 12px; border-top: 1px solid var(--border); }
.prose h3 { margin: 32px 0 10px; font-size: 20px; color: var(--text-bright); font-weight: 600; }
.prose h4 { margin: 24px 0 8px; font-size: 16px; color: var(--text-bright); font-weight: 600; }
.prose h5, .prose h6 { margin: 20px 0 6px; font-size: 15px; color: var(--text-bright); font-weight: 600; }
.prose p { margin: 0 0 14px; }
.prose ul, .prose ol { margin: 0 0 16px 22px; display: grid; gap: 6px; }
.prose li > ul, .prose li > ol { margin-top: 6px; margin-bottom: 0; }
.prose li { padding-left: 2px; }
.prose .box { display: inline-block; width: 14px; height: 14px; margin: 0 8px -2px -22px; border-radius: 4px; border: 1px solid var(--text-dim); vertical-align: baseline; }
.prose .box.done { background: var(--green); border-color: var(--green); box-shadow: 0 0 8px rgba(53,208,127,0.5); }
.prose ul li:has(> .box) { list-style: none; }
.prose blockquote { margin: 0 0 16px; padding: 12px 18px; border-left: 3px solid var(--accent); border-radius: 0 10px 10px 0; background: var(--surface); color: var(--text); }
.prose blockquote p:last-child { margin-bottom: 0; }
.prose pre { margin: 0 0 16px; padding: 16px 18px; border-radius: 12px; border: 1px solid var(--border); background: rgba(6,9,20,0.78); overflow-x: auto; font-family: var(--font-mono); font-size: 13px; line-height: 1.65; color: #c9d2e8; }
.prose pre code { padding: 0; background: none; font-size: inherit; }
.prose hr { border: 0; border-top: 1px solid var(--border); margin: 32px 0; }
.prose a { color: var(--accent); } .prose a:hover { text-decoration: underline; }
.prose img { max-width: 100%; border-radius: 12px; border: 1px solid var(--border-bright); margin: 8px 0 16px; }
.prose strong { color: var(--text-bright); }
.table { overflow-x: auto; margin: 0 0 18px; border: 1px solid var(--border); border-radius: 12px; }
table { border-collapse: collapse; width: 100%; font-size: 14px; }
th, td { padding: 9px 12px; border-bottom: 1px solid var(--border); text-align: left; vertical-align: top; }
th { color: var(--text-bright); font-weight: 600; background: var(--surface); white-space: nowrap; }
tr:last-child td { border-bottom: 0; }
.toc { margin-top: 28px; padding: 16px 20px; border-radius: 14px; border: 1px solid var(--border); background: var(--surface); font-size: 14px; }
.toc b { display: block; font-family: var(--font-mono); font-size: 12px; letter-spacing: 0.12em; text-transform: uppercase; color: var(--text-dim); margin-bottom: 8px; font-weight: 500; }
.toc ul { list-style: none; columns: 2; column-gap: 24px; }
.toc li { padding: 3px 0; break-inside: avoid; }
.toc a:hover { color: var(--accent); }
.docmeta { margin-top: 10px; font-family: var(--font-mono); font-size: 12.5px; color: var(--text-dim); display: flex; gap: 18px; flex-wrap: wrap; }
.docmeta a { color: var(--accent); }

@media (prefers-reduced-motion: reduce) { .reveal { opacity: 1; transform: none; transition: none; } .track i.next, .aurora i, h1 span, .tilt { animation: none; } html { scroll-behavior: auto; } }
@media (max-width: 860px) {
  .menu-btn { display: grid; }
  .nav-links { display: none; position: absolute; top: 64px; left: 16px; right: 16px; flex-direction: column; align-items: stretch; gap: 0; padding: 8px; border-radius: 16px; border: 1px solid var(--border); background: #0a0c19; box-shadow: 0 20px 60px rgba(0,0,0,0.6); }
  .nav.open .nav-links { display: flex; }
  .nav-links a:not(.btn) { padding: 13px 14px; border-radius: 10px; font-size: 15.5px; }
  .hero { padding-top: 64px; } .head { padding-top: 56px; }
  .g3, .g2, .split, .soon, .work, .news, .phase ul { grid-template-columns: 1fr; }
  .vow { grid-template-columns: 1fr; gap: 12px; padding: 26px 24px; }
  .tilt { transform: none; }
  .g4 { grid-template-columns: repeat(2, 1fr); }
  .layer { grid-template-columns: 1fr auto; } .layer span { grid-column: 1 / -1; grid-row: 2; }
  .closing { margin-top: 88px; padding: 52px 22px; }
  .split { gap: 32px; }
  .stats { grid-template-columns: repeat(2, 1fr); } .stat:nth-child(3) { border-left: 0; } .stat:nth-child(n+3) { border-top: 1px solid var(--border); }
  .phase-row { grid-template-columns: 70px 1fr; } .phase-row span { grid-column: 2; }
  section { padding-top: 80px; }
  .track { gap: 3px; }
  .finished a { grid-template-columns: 30px 1fr; } .finished em { display: none; }
  .phase .tag { margin-left: 0; }
  summary { grid-template-columns: 1fr 18px; gap: 6px 12px; padding: 16px 18px; }
  summary .t { grid-column: 1; grid-row: 2; } summary .chips { grid-column: 1; grid-row: 3; flex-wrap: wrap; } summary .arrow { grid-column: 2; grid-row: 1; }
  .body { padding: 4px 18px 20px; }
  .toc ul { columns: 1; }
}
"""

JS = r"""
document.querySelectorAll('.shot img').forEach(function (img) {
  function empty() { var slot = img.parentNode; slot.classList.add('empty'); slot.textContent = 'screenshot: ' + img.getAttribute('src'); }
  if (img.complete && !img.naturalWidth) empty(); else img.addEventListener('error', empty);
});
var io = new IntersectionObserver(function (entries) {
  entries.forEach(function (e) { if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); } });
}, { threshold: 0.12 });
document.querySelectorAll('.reveal').forEach(function (el) { io.observe(el); });
var tiltEl = document.querySelector('.tilt');
var calm = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
function tilt() { if (tiltEl) tiltEl.style.setProperty('--tilt', (calm ? 0 : Math.max(0, 9 - scrollY / 45)).toFixed(2) + 'deg'); }
var progress = document.getElementById('progress');
function onScroll() {
  var max = document.documentElement.scrollHeight - innerHeight;
  progress.style.transform = 'scaleX(' + (max > 0 ? scrollY / max : 0) + ')';
  tilt();
}
addEventListener('scroll', onScroll, { passive: true }); onScroll();
document.addEventListener('pointermove', function (e) {
  var card = e.target.closest && e.target.closest('.card'); if (!card) return;
  var r = card.getBoundingClientRect();
  card.style.setProperty('--mx', (e.clientX - r.left) + 'px'); card.style.setProperty('--my', (e.clientY - r.top) + 'px');
});
var stats = document.querySelector('.stats');
if (stats) new IntersectionObserver(function (entries, obs) {
  if (!entries[0].isIntersecting) return; obs.disconnect(); if (calm) return;
  document.querySelectorAll('[data-count]').forEach(function (el) {
    var end = +el.dataset.count, t0 = performance.now();
    (function tick(now) { var p = Math.min(1, (now - t0) / 1100); el.textContent = Math.round(end * (1 - Math.pow(1 - p, 3))).toLocaleString(); if (p < 1) requestAnimationFrame(tick); })(t0);
  });
}, { threshold: 0.6 }).observe(stats);
var nav = document.querySelector('.nav'), menu = document.getElementById('menu');
function setMenu(open) { nav.classList.toggle('open', open); menu.setAttribute('aria-expanded', open); }
menu.addEventListener('click', function () { setMenu(!nav.classList.contains('open')); });
nav.querySelectorAll('.nav-links a').forEach(function (a) { a.addEventListener('click', function () { setMenu(false); }); });
var lightbox = document.getElementById('lightbox');
document.querySelectorAll('.shot img').forEach(function (img) {
  img.addEventListener('click', function () { lightbox.firstElementChild.src = img.src; lightbox.classList.add('open'); });
});
lightbox.addEventListener('click', function () { lightbox.classList.remove('open'); });
document.addEventListener('keydown', function (e) { if (e.key === 'Escape') lightbox.classList.remove('open'); });
var list = document.getElementById('list'), toggle = document.getElementById('toggle');
if (list && toggle) {
  var all = Array.prototype.slice.call(list.querySelectorAll('details'));
  function sync() { toggle.textContent = all.every(function (d) { return d.open; }) ? 'Collapse all' : 'Expand all'; }
  toggle.addEventListener('click', function () { var open = !all.every(function (d) { return d.open; }); all.forEach(function (d) { d.open = open; }); sync(); });
  all.forEach(function (d) { d.addEventListener('toggle', sync); });
  function openHash() { var d = location.hash && document.getElementById(location.hash.slice(1)); if (d && d.tagName === 'DETAILS') { d.open = true; d.scrollIntoView(); sync(); } }
  window.addEventListener('hashchange', openHash); openHash();
}
"""

ICON = "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' rx='24' fill='%2306070f'/><circle cx='50' cy='50' r='20' fill='%235aa2ff'/></svg>"

NAV = [("/", "Home"), ("/#today", "Today"), ("/roadmap/", "Roadmap"), ("/roadmap/#releases", "Releases"), ("/docs/", "Docs"), (REPO_URL, "GitHub")]


def shell(title, description, path, body, body_class, version, image="/img/screenshots/settings.png"):
    nav = "".join('<a href="%s"%s>%s</a>' % (h, ' class="on"' if h == path else "", t) for h, t in NAV)
    return """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>%(title)s</title>
  <meta name="description" content="%(desc)s" />
  <meta property="og:title" content="%(title)s" />
  <meta property="og:description" content="%(desc)s" />
  <meta property="og:type" content="website" />
  <meta property="og:url" content="%(base)s%(path)s" />
  <meta property="og:image" content="%(base)s%(image)s" />
  <link rel="canonical" href="%(base)s%(path)s" />
  <link rel="manifest" href="/manifest.json" />
  <meta name="theme-color" content="#06070f" />
  <link rel="icon" href="%(icon)s" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet" />
  <link rel="stylesheet" href="/site.css" />
</head>
<body class="%(cls)s">
<div class="aurora" aria-hidden="true"><i></i><i></i><i></i></div>
<div class="progress" id="progress"></div>
<header class="nav">
  <div class="wrap">
    <a class="wordmark" href="/" aria-label="Cerberus OS home">Cerberus<i></i></a>
    <button class="menu-btn" id="menu" type="button" aria-label="Menu" aria-expanded="false"><span></span></button>
    <nav class="nav-links">%(nav)s</nav>
  </div>
</header>
<main id="top">
  <div class="wrap">
%(body)s
  </div>
</main>
<div class="lightbox" id="lightbox" role="dialog" aria-label="Screenshot preview"><img alt="" /></div>
<footer>
  <div class="wrap">
    <span>Cerberus OS &middot; v%(version)s &middot; this page was generated from the repository's docs on %(today)s</span>
    <nav>
      <a href="/">Home</a>
      <a href="/roadmap/">Roadmap</a>
      <a href="/docs/">Docs</a>
      <a href="%(repo)s">Source</a>
    </nav>
  </div>
</footer>
<script src="/site.js"></script>
</body>
</html>
""" % {"title": html.escape(title), "desc": html.escape(description, quote=True), "base": BASE_URL, "path": path,
       "image": image, "icon": ICON, "cls": body_class, "nav": nav, "body": body, "version": version,
       "today": TODAY, "repo": REPO_URL}


def esc(s):
    return html.escape(s, quote=False)


def num(v):
    """('1,394', 'ns') → ('1,394', 'ns') with a data-count for the counter animation."""
    return v


def stat(value, unit, label):
    if value is None:
        return ""
    clean = value.replace(",", "")
    try:
        float(clean)
        inner = '<span data-count="%s">%s</span>' % (clean, value)
    except ValueError:
        inner = esc(value)
    return '<div class="stat"><strong>%s<small>%s</small></strong><span>%s</span></div>' % (inner, unit, label)


def release_body(r):
    order = ["Security", "Fixed", "Performance", "Added", "Changed", "Removed"]
    parts = []
    if r["intro"]:
        parts.append("<p>%s</p>" % md_inline(r["intro"]))
    for g in order:
        if g in r["groups"]:
            parts.append('<h4 class="%s">%s</h4><ul>%s</ul>' % (
                g.lower(), g, "".join("<li>%s</li>" % md_inline(l) for l in r["groups"][g])))
    return "".join(parts)


def release_details(r, chips, open_=False):
    title = first_sentence(plain(r["intro"]), 110) if r["intro"] else "Release"
    if r["groups"] and not r["intro"]:
        first = next(iter(r["groups"].values()))[0]
        title = first_sentence(plain(first), 110)
    return ('<details id="v%s"%s><summary><span class="v">%s</span><span class="t">%s<small>%s</small></span>'
            '<span class="chips">%s</span><span class="arrow"></span></summary><div class="body">%s</div></details>') % (
        r["v"], " open" if open_ else "", esc(r["v"]), esc(title), esc(r["date"] or "in development"), chips, release_body(r))


def build():
    version_next = (ROOT / "VERSION").read_text().strip()
    releases, unreleased = parse_changelog()
    first_tagged = next(i for i, r in enumerate(releases) if r["tagged"])
    latest = releases[first_tagged]
    pending = releases[:first_tagged]  # newer than the last tag: built and in the changelog, not released
    for r in releases[first_tagged:]:
        r["tagged"] = True  # old snapshots before tags existed (0.0.1) count as released
    phases, done_phases = parse_phases()
    roadmap = parse_roadmap_phases()
    metrics, bench_title = parse_bench()
    features = parse_features()
    open_bugs = parse_bugs()
    n_syscalls = count_syscalls()
    n_tests = count_tests()

    numeric = [p for p in phases if p["n"].isdigit()]
    next_phase = next((p for p in numeric if not p["done"]), None)
    last_done = max([d["n"] for d in done_phases] + [int(p["n"]) for p in numeric if p["done"]])
    next_n = int(next_phase["n"]) if next_phase else last_done + 1
    in_dev = bool(unreleased and unreleased["groups"]) or bool(pending)

    # ---- shared pieces
    track = "".join('<i class="%s"></i>' % ("done" if i <= last_done else "next" if i == next_n else "") for i in range(20))
    screenshots = sorted((DOCS / "screenshots").glob("*.png"))
    hero_shot = next((s for s in screenshots if s.stem == "settings"), screenshots[0] if screenshots else None)
    others = [s for s in screenshots if s != hero_shot]
    captions = {"launcher": "The launcher with app search and the floating taskbar",
                "calendar": "The calendar with a reminder, opened from the taskbar clock",
                "snap": "Dragging a window to the screen edge: the snap outline",
                "settings": "Settings: a custom accent colour, the Aurora wallpaper and the Chase window border"}

    # ---- home
    cs = metric(metrics, "Context switch")
    sc = metric(metrics, "System-call")
    pf = metric(metrics, "Minor page fault")
    km = metric(metrics, "kmalloc")
    wk = metric(metrics, "Wake-up latency, average", "Wake-up latency")
    idle = metric(metrics, "Idle desktop")
    stats = stat(cs[0], cs[1], "context switch") if cs else ""
    stats += stat(sc[0], sc[1], "system call") if sc else ""
    stats += stat(idle[0], idle[1], "CPU on an idle desktop") if idle else ""
    stats += stat(str(n_syscalls), "", "system calls")

    cards = []
    skip = {"Tooling", "Not there yet"}
    for a in features:
        if a["title"] in skip or not a["bullets"]:
            continue
        cards.append('<article class="card slim reveal"><h3>%s</h3><p>%s</p><a class="more" href="/docs/features/#%s">All of it &rarr;</a></article>' % (
            esc(a["title"]), md_inline(first_sentence(plain(a["bullets"][0]), 170)), a["id"]))
    not_yet = next((a for a in features if a["title"] == "Not there yet"), None)

    news = []
    for g in ["Security", "Fixed", "Performance", "Added", "Changed", "Removed"]:
        if g in latest["groups"]:
            items = latest["groups"][g][:5]
            news.append('<div class="card"><h4 class="%s">%s</h4><ul>%s</ul>%s</div>' % (
                g.lower(), g, "".join("<li>%s</li>" % md_inline(l) for l in items),
                '<a class="more" href="/roadmap/#v%s">and %d more &rarr;</a>' % (latest["v"], len(latest["groups"][g]) - 5) if len(latest["groups"][g]) > 5 else ""))

    measured = ""
    for v, label in [(pf, "first touch of a memory page (minor page fault)"), (wk, "to wake an interactive thread"),
                     (km, "for a kernel heap allocation and free"), (metric(metrics, "Last composite"), "to composite a desktop frame")]:
        if v:
            measured += '<article class="card metric reveal"><strong>%s<small>%s</small></strong><p>%s</p></article>' % (esc(v[0]), v[1], label)

    phase_rows = ""
    for p in phases:
        if p["done"]:
            continue
        up = p["n"] == (next_phase["n"] if next_phase else "")
        phase_rows += '<div class="phase-row%s reveal"><code>%s%s</code><b>%s</b><span>%s</span></div>' % (
            " up" if up else "", esc(p["n"]), " &middot; next" if up else "", esc(p["title"]), md_inline(p["text"]))

    pill = "v%s &middot; phases 0&ndash;%d done &middot; phase %d next" % (latest["v"], last_done, next_n)
    if pending:
        pill += " &middot; %s built, release pending" % pending[0]["v"]
    elif in_dev:
        pill += " &middot; %s in development" % version_next

    gallery = "".join('<figure class="shot reveal"><img src="/img/screenshots/%s" alt="%s" width="1280" height="800" loading="lazy" /><figcaption>%s</figcaption></figure>' % (
        s.name, esc(captions.get(s.stem, s.stem)), esc(captions.get(s.stem, s.stem))) for s in others[:4])

    home = """
    <div class="hero">
      <span class="pill"><b></b>%(pill)s</span>
      <h1>An operating system, <span>written from scratch.</span></h1>
      <p class="lead">Cerberus is a 64-bit OS with its own kernel, its own file system, its own drivers and its own desktop. Nothing here is Linux, BSD or Windows underneath.</p>
      <div class="cta">
        <a class="btn primary" href="#today">See what works</a>
        <a class="btn" href="/roadmap/">Roadmap</a>
        <a class="btn" href="%(repo)s">Source on GitHub</a>
      </div>
      %(hero)s
      <div class="stats reveal">%(stats)s</div>
    </div>

    <section id="why">
      <span class="eyebrow">Why</span>
      <h2>Three goals, in this order.</h2>
      <div class="grid g3">
        <article class="card reveal"><span class="num">01</span><h3>Private and secure by design</h3><p>No telemetry. Nothing leaves the machine unless you ask. Memory is never writable and executable at once, and every system-call argument is treated as hostile.</p></article>
        <article class="card reveal"><span class="num">02</span><h3>Fast and small</h3><p>A message between two programs takes well under a microsecond. Every performance budget is measured and recorded, so a regression is visible.</p></article>
        <article class="card reveal"><span class="num">03</span><h3>Understandable</h3><p>One small system, with a written specification and the reasoning behind every decision, all published in <a href="/docs/">the docs</a>.</p></article>
      </div>
    </section>

    <section id="today">
      <span class="eyebrow">What works today</span>
      <h2>A real system, from the boot sector to the window shadows.</h2>
      <p class="sub">It runs in QEMU and VirtualBox today, brings up every CPU core, and runs protected user programs. Each card is the first line of its section in <a href="/docs/features/">the feature inventory</a>, which lists everything.</p>
      <div class="grid g3">%(cards)s</div>
      %(not_yet)s
    </section>

    <section id="new">
      <span class="eyebrow">What's new</span>
      <h2>Version %(lv)s, released %(ldate)s.</h2>
      <p class="sub">%(lintro)s</p>
      <div class="news reveal">%(news)s</div>
      <div class="cta" style="justify-content:flex-start;margin-top:24px"><a class="btn small" href="/roadmap/#v%(lv)s">The full release notes</a><a class="btn small" href="/roadmap/#releases">Every release</a></div>
    </section>

    <section id="security">
      <span class="eyebrow">Security and privacy</span>
      <h2>Nothing leaves the machine unless you ask.</h2>
      <p class="sub">In Cerberus, security rules are requirements with tests, not afterthoughts. Every one of them below is enforced in the current build.</p>
      <div class="vow reveal"><span class="zero">0</span><div><h3>Zero telemetry.</h3><p>Cerberus collects nothing and reports nothing. No usage data, no account to sign in to, no background phone-home. Today it has no network stack at all.</p></div></div>
      <div class="grid g3" style="margin-top:16px">
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg></span><h3>Never writable and executable</h3><p>No memory can be both at once. Every request for such a mapping is refused, in the kernel and in every program.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6z"/><path d="M9 12l2 2 4-4"/></svg></span><h3>Every argument is hostile</h3><p>Each system-call argument is checked. Bad pointers, lengths that wrap and unknown flags get an error, never kernel code.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><rect x="7" y="7" width="10" height="10" rx="2"/><path d="M10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4"/></svg></span><h3>Walls in the processor</h3><p>SMEP, SMAP and UMIP are on wherever the CPU has them: the kernel can neither run nor casually touch program memory.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><path d="M4 7h10l-3-3M20 17H10l3 3M4 17h2M18 7h2"/></svg></span><h3>A different layout every run</h3><p>A program's code, stack and mapped memory land at different addresses each time it starts.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><path d="M5 20h14M7 20l1-11h8l1 11M9 9V6a3 3 0 0 1 6 0v3"/></svg></span><h3>Nothing left behind</h3><p>Memory is zeroed before a program gets it, and a new disk block never shows what a deleted file left in it.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><circle cx="12" cy="8" r="3.5"/><path d="M5 20c1-4 4-6 7-6s6 2 7 6"/></svg></span><h3>Identity that cannot be faked</h3><p>A program receiving a message learns the sender's process and user from the kernel, not from the message.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><ellipse cx="12" cy="6" rx="7" ry="3"/><path d="M5 6v12c0 1.7 3.1 3 7 3s7-1.3 7-3V6M5 12c0 1.7 3.1 3 7 3s7-1.3 7-3"/></svg></span><h3>Disks are not trusted</h3><p>cerfs checks every field and checksums its metadata. A damaged disk gives an error, never a crash.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><path d="M4 12h3l2-6 4 12 2-6h5"/></svg></span><h3>Attacked on every release</h3><p>Fuzzers hammer the program loader, the archive reader, the file system and the system-call interface; %(tests)d integration tests run on every build.</p></article>
        <article class="card reveal"><span class="ico"><svg viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="3"/><circle cx="9" cy="9" r="1"/><circle cx="15" cy="15" r="1"/><circle cx="15" cy="9" r="1"/><circle cx="9" cy="15" r="1"/></svg></span><h3>Real randomness</h3><p>A ChaCha20 generator in the kernel, seeded from the CPU's hardware generator and interrupt timing.</p></article>
      </div>
      <p class="minor">Planned, and already written into the specification</p>
      <div class="soon reveal">
        <div><code>phase 12</code><span><b>Private windows.</b> Screenshots, screen recording and clipboard history need your permission per app, denied by default.</span></div>
        <div><code>phase 14</code><span><b>Quiet network.</b> Incoming connections blocked by default, and nothing sent at boot except DHCP.</span></div>
        <div><code>phase 15</code><span><b>Sandboxed apps.</b> Each app declares what it needs and can be denied network, files or devices.</span></div>
        <div><code>phase 18</code><span><b>Your disk, your updates.</b> Optional full-disk encryption, and signed updates that never install on their own.</span></div>
      </div>
      %(bugs)s
    </section>

    <section id="design">
      <div class="split">
        <div>
          <span class="eyebrow">Architecture</span>
          <h2>A hybrid kernel, in clear layers.</h2>
          <p>Anything that needs sub-microsecond speed or special processor instructions stays in the kernel. Everything else runs as an ordinary, protected program.</p>
          <p style="margin-top:14px">Each layer only talks to the one below it through well-defined, checked doors. The plain-language tour is in <a href="/docs/about/">About Cerberus</a>.</p>
        </div>
        <div class="stack reveal">
          <div class="layer"><b>Applications</b><span>Terminal, file manager, editor</span><em>phase 13</em></div>
          <div class="layer"><b>Servers</b><span>Pane (windows), netd (network)</span><em>phases 12, 14</em></div>
          <div class="layer live"><b>Kernel</b><span>Memory, scheduling, system calls, files, drivers, messaging, and a preview desktop</span><em>working today</em></div>
          <div class="layer hw"><b>Hardware</b><span>Processor cores, memory, disk, screen</span><em>x86-64</em></div>
        </div>
      </div>
    </section>

    <section id="measured">
      <span class="eyebrow">Measured</span>
      <h2>Every budget is measured and written down.</h2>
      <p class="sub">The newest block in <a href="/docs/bench/">the benchmark record</a> (%(bench)s), on QEMU/KVM with 4 cores. Each number is checked against a written budget.</p>
      <div class="grid g4">%(measured)s</div>
    </section>

    <section id="desktop">
      <div class="split">
        <div>
          <span class="eyebrow">The desktop</span>
          <h2>The desktop, so far.</h2>
          <p>A floating taskbar, a launcher with app search, a calendar with reminders, a lock screen, virtual desktops, and a Settings window that changes the look live. Today it is a preview that runs inside the kernel; phase 12 moves it into its own protected program.</p>
          <div class="keys">
            <div><span><kbd>Super</kbd></span>Open the launcher</div>
            <div><span><kbd>Super</kbd><kbd>T</kbd></span>Open a terminal</div>
            <div><span><kbd>Alt</kbd><kbd>Tab</kbd></span>Switch windows</div>
            <div><span><kbd>Super</kbd><kbd>&larr;</kbd><kbd>&rarr;</kbd></span>Snap a window to half the screen</div>
            <div><span><kbd>Super</kbd><kbd>1</kbd>&hellip;<kbd>4</kbd></span>Switch virtual desktop</div>
            <div><span><kbd>Super</kbd><kbd>L</kbd></span>Lock the screen</div>
            <div><span><kbd>Print</kbd></span>Save a screenshot</div>
          </div>
        </div>
        <figure class="shot reveal"><img src="/img/screenshots/%(hero_name)s" alt="%(hero_alt)s" width="1280" height="800" loading="lazy" /><figcaption>%(hero_alt)s</figcaption></figure>
      </div>
      <div class="grid g3" style="margin-top:16px">%(gallery)s</div>
      <p class="note">Everything the desktop can do, key by key, is in <a href="/docs/features/#desktop-kernel-hosted-preview-of-pane">the feature inventory</a>.</p>
    </section>

    <section id="roadmap">
      <span class="eyebrow">Roadmap</span>
      <h2>Built in numbered phases. 0 to %(last_done)d are done.</h2>
      <p class="sub">Each phase ends in a tested release, and no phase starts until the one before it passes its acceptance tests.</p>
      <div class="track reveal" aria-hidden="true">%(track)s</div>
      <div class="track-label"><span>phase 0</span><span>phase %(next_n)d next</span><span>phase 19</span></div>
      <div class="phases">%(phase_rows)s</div>
      <div class="cta" style="margin-top:36px"><a class="btn" href="/roadmap/">Read the full roadmap</a></div>
    </section>

    <section id="spec">
      <div class="split">
        <div>
          <span class="eyebrow">Spec</span>
          <h2>A language designed with its operating system.</h2>
          <p>Spec is the systems language planned for writing Cerberus's own programs. Compiled to native code, statically typed, no garbage collector.</p>
          <ul>
            <li><b>Regions.</b> Memory allocated in a region is freed at its closing brace.</li>
            <li><b>Typed system calls.</b> Part of the language, with no C layer between.</li>
            <li><b>Capability types.</b> A handle cannot be copied by accident or used after it is closed.</li>
            <li><b>Checked register layouts.</b> The compiler refuses a write to a read-only register.</li>
          </ul>
        </div>
        <div class="reveal">
          <div class="code">
            <header><span>ahci.spec</span><span>planned syntax</span></header>
<pre><span class="k">trait</span> <span class="t">Show</span> { <span class="k">fn</span> show(self) -> <span class="t">str</span> }
<span class="k">fn</span> dump[<span class="t">T</span>: <span class="t">Show</span>](v: <span class="t">T</span>)

<span class="k">region</span> r { <span class="k">let</span> x = r.alloc(...); }

<span class="k">syscall</span> write(fd: <span class="t">Fd</span>, buf: &amp;[<span class="t">u8</span>]) -> <span class="t">Result</span>[<span class="t">usize</span>, <span class="t">Errno</span>]

<span class="k">mmio struct</span> <span class="t">AhciPort</span> {
    0x00: clb: <span class="t">u32</span> rw,
    0x10: is:  <span class="t">u32</span> rw1c,
}</pre>
          </div>
          <p class="note">The compiler has not been started (phase 16) and the syntax is not final. Until then the system is freestanding C++20.</p>
        </div>
      </div>
    </section>

    <div class="closing reveal">
      <span class="wordmark">Cerberus<i></i></span>
      <h2>Next: phase %(next_n)d, %(next_title)s.</h2>
      <p>%(next_text)s</p>
      <div class="cta">
        <a class="btn primary" href="/roadmap/">Read the full roadmap</a>
        <a class="btn" href="/docs/status/">What is verified right now</a>
      </div>
    </div>
""" % {
        "pill": pill, "repo": REPO_URL, "stats": stats, "cards": "".join(cards),
        "hero": ('<figure class="shot hero-shot reveal"><div class="tilt"><img src="/img/screenshots/%s" alt="%s" width="1280" height="800" /></div></figure>' % (
            hero_shot.name, esc(captions.get(hero_shot.stem, "The Cerberus desktop")))) if hero_shot else "",
        "not_yet": ('<p class="note" style="margin-top:28px"><b>Not there yet:</b> %s</p>' % md_inline(" ".join(not_yet["text"].split()))) if not_yet else "",
        "lv": esc(latest["v"]), "ldate": esc(latest["date"] or ""), "lintro": md_inline(latest["intro"]) if latest["intro"] else "The release notes, straight from the changelog.",
        "news": "".join(news), "tests": n_tests,
        "bugs": ('<p class="minor">Open bugs, from the ledger</p><div class="work reveal">%s</div>' % "".join(
            '<div><b><a href="/docs/bugs/#%s">%s</a></b> %s</div>' % (slugify("%s — %s" % (b, s)), b, md_inline(s)) for b, s in open_bugs)) if open_bugs else
            '<p class="minor">The bug ledger has no open entries</p>',
        "bench": esc(bench_title), "measured": measured,
        "hero_name": hero_shot.name if hero_shot else "", "hero_alt": esc(captions.get(hero_shot.stem, "")) if hero_shot else "",
        "gallery": gallery, "last_done": last_done, "track": track, "next_n": next_n, "phase_rows": phase_rows,
        "next_title": esc(next_phase["title"].split(":")[0]) if next_phase else "", "next_text": md_inline(next_phase["text"]) if next_phase else "",
    }

    # ---- roadmap page
    def phase_card(p, cls, tag):
        rp = roadmap.get(p["n"])
        items = rp["items"] if rp else []
        shown = items[:8]
        lis = "".join('<li class="%s">%s</li>' % ("done" if it["done"] else "", md_inline(it["text"])) for it in shown)
        more = ('<p class="note">%d more items in <a href="/docs/roadmap/#%s">the roadmap document</a>.</p>' % (
            len(items) - 8, slugify("Phase %s — %s" % (rp["n"], rp["title"])))) if len(items) > 8 else ""
        goal = ('<p class="goal"><b>Done when:</b> %s</p>' % md_inline(rp["goal"])) if rp and rp["goal"] else ""
        if rp and rp["decision"]:
            goal += '<p class="goal"><b>Owner decision first:</b> %s</p>' % md_inline(rp["decision"])
        intro = md_inline(rp["intro"]) if rp and rp["intro"] else md_inline(p["text"])
        title = rp["title"] if rp else p["title"]
        return ('<article class="phase %s" id="phase-%s"><header><span class="n">Phase %s</span><h3>%s</h3><span class="tag">%s</span></header>'
                '<p>%s</p>%s%s%s</article>') % (cls, p["n"], esc(p["n"]), esc(title), tag, intro,
                                                 "<ul>%s</ul>" % lis if lis else "", more, goal)

    ahead = "".join(phase_card(p, "", "stretch" if p["n"] == "19" else "planned") for p in phases if not p["done"] and p is not next_phase)
    now_card = phase_card(next_phase, "next", "next") if next_phase else ""

    # "also on the workbench": the Right now section of TO_FINISH, first paragraph
    tf = read("TO_FINISH.md")
    m = re.search(r"^## Right now:?\s*(.*?)$\s+(.*?)(?=\n\n)", tf, re.S | re.M)
    right_now = (esc(m.group(1).strip()), md_inline(" ".join(m.group(2).split()))) if m else ("", "")
    unreleased_html = ""
    pend = ""
    for r in pending:
        pend += release_details(dict(r, date="built %s, not yet released" % (r["date"] or "")),
                                '<span class="chip dev">release pending</span>' + ('<span class="chip sec">security</span>' if r["security"] else ""), True)
    if unreleased and unreleased["groups"]:
        pend += release_details({"v": version_next, "date": "", "intro": unreleased["intro"], "groups": unreleased["groups"]},
                                '<span class="chip dev">unreleased</span>', True)
    if pend:
        unreleased_html = '<p class="minor">Built, not yet released (no release tag; the ISO in dist/ is still v%s)</p><div class="releases">%s</div>' % (esc(latest["v"]), pend)

    finished = "".join('<a href="#v%s"><code>%d</code><span><b>%s.</b> %s</span><em>%s</em></a>' % (
        d["v"], d["n"], esc(d["title"]), md_inline(d["text"]), esc(d["v"])) for d in done_phases)

    rel_html = ""
    for i, r in enumerate(r for r in releases if r["tagged"]):
        chips = ""
        if i == 0:
            chips += '<span class="chip latest">latest</span>'
        if r["phase"]:
            chips += '<span class="chip ph">%s</span>' % esc(r["phase"])
        if r["security"]:
            chips += '<span class="chip sec">security</span>'
        rel_html += release_details(r, chips, i == 0)

    roadmap_page = """
    <div class="head">
      <span class="eyebrow">Roadmap</span>
      <h1>Where Cerberus is going, <span>and how it got here.</span></h1>
      <p class="lead">Cerberus is built in numbered phases. Each one ends in a tested release, and no phase starts until the one before it passes its acceptance tests. This page is generated from <a href="/docs/to_finish/">the checklist</a>, <a href="/docs/roadmap/">the roadmap document</a> and <a href="/docs/changelog/">the changelog</a>.</p>
      <div class="jump">
        <a class="btn small" href="#now">Next up</a>
        <a class="btn small" href="#ahead">What's coming</a>
        <a class="btn small" href="#done">Completed</a>
        <a class="btn small" href="#releases">Release history</a>
      </div>
      <div class="track" aria-hidden="true">%(track)s</div>
      <div class="track-label"><span>phases 0 to %(last_done)d done</span><span>phase %(next_n)d next &middot; v%(lv)s released</span></div>
    </div>

    <section id="now">
      <span class="eyebrow">Next up</span>
      <h2>What is being built next.</h2>
      <p class="sub">The newest release is v%(lv)s (%(ldate)s). %(rn_title)s</p>
      <div class="ahead">%(now)s</div>
      <p class="minor">Right now, from the checklist</p>
      <div class="work"><div>%(rn_text)s</div></div>
    </section>

    <section id="ahead">
      <span class="eyebrow">What's coming</span>
      <h2>The road from here.</h2>
      <p class="sub">Every phase after the next one, in the order it will be built, with its items from the roadmap document.</p>
      <div class="ahead">%(ahead)s</div>
    </section>

    <section id="done">
      <span class="eyebrow">Completed</span>
      <h2>%(n_done)d phases done.</h2>
      <p class="sub">Every finished phase passed its acceptance tests before the next one started. Pick one to open the release that completed it.</p>
      <div class="finished">%(finished)s</div>
    </section>

    <section id="releases">
      <div class="bar">
        <div>
          <span class="eyebrow">Release history</span>
          <h2>Every version so far.</h2>
          <p class="sub">From the first snapshot to the latest release, with the real dates, straight from <a href="/docs/changelog/">the changelog</a>. Pick a version to see what it changed.</p>
        </div>
        <button class="btn" id="toggle" type="button">Expand all</button>
      </div>
      %(unreleased)s
      <div class="releases" id="list">%(releases)s</div>
    </section>
""" % {"track": track, "last_done": last_done, "next_n": next_n, "lv": esc(latest["v"]), "ldate": esc(latest["date"] or ""),
       "rn_title": right_now[0], "rn_text": right_now[1], "now": now_card, "ahead": ahead,
       "n_done": len(done_phases), "finished": finished, "unreleased": unreleased_html, "releases": rel_html}

    # ---- docs pages
    doc_pages = []
    for md_file in sorted(DOCS.glob("*.md")):
        name = md_file.stem
        text = md_file.read_text(encoding="utf-8")
        r = Md(text)
        body_html = r.render()
        h1 = next((h for h in r.headings if h[0] == 1), None)
        title = h1[1] if h1 else name
        blurb = DOC_BLURBS.get(name)
        if not blurb:
            m = re.search(r"\*\*What this file is for\.\*\*\s*(.*?)(?:\n\n|$)", text, re.S)
            blurb = first_sentence(plain(m.group(1).replace("\n", " ")), 160) if m else ""
        toc = [h for h in r.headings if h[0] == 2]
        toc_html = ('<nav class="toc"><b>On this page</b><ul>%s</ul></nav>' % "".join(
            '<li><a href="#%s">%s</a></li>' % (h[2], md_inline(h[1])) for h in toc)) if len(toc) > 2 else ""
        words = len(re.findall(r"\w+", text))
        page = """
    <div class="head">
      <span class="eyebrow">Docs &middot; %(file)s</span>
      <div class="docmeta"><span>%(words)s words</span><span>updated with every commit</span><a href="%(repo)s/blob/main/docs/%(file)s">View the source on GitHub</a></div>
      %(toc)s
    </div>
    <article class="prose">%(body)s</article>
""" % {"file": esc(md_file.name), "words": "{:,}".format(words), "repo": REPO_URL, "toc": toc_html, "body": body_html}
        doc_pages.append((name, title, blurb, page))

    order = {n: i for i, n in enumerate(DOC_ORDER)}
    doc_pages.sort(key=lambda d: order.get(d[0], 99))
    doc_cards = "".join('<a class="card doc reveal" href="/docs/%s/"><div class="path">docs/%s.md</div><h3>%s</h3><p>%s</p></a>' % (
        n.lower(), esc(n), esc(plain(t)), esc(b)) for n, t, b, _ in doc_pages)
    docs_index = """
    <div class="head">
      <span class="eyebrow">Docs</span>
      <h1>Every document, <span>as it is in the repository.</span></h1>
      <p class="lead">These pages are rendered from the Markdown files in <code>docs/</code> on every update, so they say exactly what the project's own records say. The specification wins over every other document; the status file says what was verified and when.</p>
    </div>
    <div class="grid g3">%s</div>
""" % doc_cards

    # ---- write everything
    SITE.mkdir(parents=True, exist_ok=True)
    (SITE / "site.css").write_text(CSS.strip() + "\n", encoding="utf-8")
    (SITE / "site.js").write_text(JS.strip() + "\n", encoding="utf-8")
    (SITE / "index.html").write_text(shell(
        "Cerberus OS — a 64-bit operating system written from scratch",
        "Cerberus is a 64-bit operating system written from scratch: its own kernel, file system, drivers and desktop. Private, fast, and small enough to understand.",
        "/", home, "home", latest["v"]), encoding="utf-8")
    (SITE / "roadmap").mkdir(exist_ok=True)
    (SITE / "roadmap" / "index.html").write_text(shell(
        "Roadmap and release history — Cerberus OS",
        "Where Cerberus OS is going, phase by phase, and every release from the first snapshot to the latest version.",
        "/roadmap/", roadmap_page, "narrow", latest["v"]), encoding="utf-8")
    docs_dir = SITE / "docs"
    if docs_dir.exists():
        shutil.rmtree(docs_dir)
    docs_dir.mkdir()
    (docs_dir / "index.html").write_text(shell(
        "Documentation — Cerberus OS", "The Cerberus OS documents, rendered from the repository: about, features, status, roadmap, changelog, bugs, benchmarks, decisions and the specification.",
        "/docs/", docs_index, "home", latest["v"]), encoding="utf-8")
    urls = ["/", "/roadmap/", "/docs/"]
    for n, t, b, page in doc_pages:
        d = docs_dir / n.lower()
        d.mkdir()
        (d / "index.html").write_text(shell("%s — Cerberus OS" % plain(t), b or plain(t), "/docs/%s/" % n.lower(), page, "narrow", latest["v"]), encoding="utf-8")
        urls.append("/docs/%s/" % n.lower())
    img = SITE / "img" / "screenshots"
    if img.exists():
        shutil.rmtree(img)
    img.mkdir(parents=True)
    for s in screenshots:
        shutil.copy2(s, img / s.name)
    for stale in ["hero.png", "shot-1.png", "shot-2.png"]:
        p = SITE / "img" / stale
        if p.exists():
            p.unlink()
    (SITE / "sitemap.xml").write_text('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n%s</urlset>\n' % "".join(
        "  <url><loc>%s%s</loc><lastmod>%s</lastmod></url>\n" % (BASE_URL, u, TODAY) for u in urls), encoding="utf-8")
    (SITE / "robots.txt").write_text("User-agent: *\nAllow: /\n\nSitemap: %s/sitemap.xml\n" % BASE_URL, encoding="utf-8")
    (SITE / "404.html").write_text("""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Not found — Cerberus OS</title>
  <meta name="robots" content="noindex" />
  <style>
    body { margin: 0; min-height: 100vh; display: grid; place-items: center; text-align: center; background: #06070f radial-gradient(800px 500px at 80% 0, rgba(40,90,200,0.35), transparent 65%); color: #aab1c5; font-family: Inter, -apple-system, 'Segoe UI', sans-serif; }
    h1 { margin: 0 0 8px; font-size: 72px; letter-spacing: -0.04em; color: #f1f3f9; }
    code { font-family: Consolas, monospace; color: #5aa2ff; }
    a { display: inline-block; margin-top: 24px; padding: 10px 18px; border-radius: 10px; background: #2f6bf2; color: #fff; text-decoration: none; font-weight: 600; font-size: 14px; }
  </style>
</head>
<body><main><h1>404</h1><p><code>cerberus&gt;</code> no such file or directory</p><a href="/">Back to Cerberus OS</a></main></body>
</html>
""", encoding="utf-8")
    (SITE / "manifest.json").write_text("""{
  "name": "Cerberus OS",
  "short_name": "Cerberus",
  "description": "A 64-bit operating system written from scratch.",
  "start_url": "/",
  "display": "standalone",
  "background_color": "#06070f",
  "theme_color": "#06070f",
  "icons": [{ "src": "%s", "sizes": "any", "type": "image/svg+xml" }]
}
""" % ICON, encoding="utf-8")
    print("site: %s (v%s released%s; VERSION %s; phases 0-%d done, next %d; %d docs, %d screenshots, %d releases)" % (
        SITE, latest["v"], ", %s built but untagged" % ", ".join(p["v"] for p in pending) if pending else "",
        version_next, last_done, next_n, len(doc_pages), len(screenshots), len(releases)))


if __name__ == "__main__":
    build()
