"""HTML triptych gallery for the MC2 visual comparator (artifact browser v1).

One row per bookmark: baseline | candidate | diff-heatmap. All images are
inlined as base64 data URIs so the output is a single self-contained .html the
human opens. Diff images are for HUMAN eyes only -- agents read visual_diff.json.

Python 3 stdlib only (base64). The heatmap is built from already-decoded pixel
buffers (see png_io) so this module does no PNG parsing itself.
"""

import base64

from png_io import encode_png


def build_heatmap(width, height, base_rgb, cand_rgb, gain=8):
    """Build an RGB heatmap PNG (bytes) where brightness ~ per-pixel delta.

    base_rgb / cand_rgb are flat RGB bytearrays of equal length. Delta magnitude
    = sum of absolute per-channel differences, scaled by ``gain`` and clamped to
    255. Unchanged pixels are black; larger deltas glow brighter (red->yellow).
    """
    n = width * height
    out = bytearray(n * 3)
    for p in range(n):
        i = p * 3
        d = (
            abs(base_rgb[i] - cand_rgb[i])
            + abs(base_rgb[i + 1] - cand_rgb[i + 1])
            + abs(base_rgb[i + 2] - cand_rgb[i + 2])
        )
        v = d * gain
        if v > 255:
            v = 255
        # red ramps first, green follows for hot spots -> red->orange->yellow
        out[i] = v
        out[i + 1] = v // 2
        out[i + 2] = 0
    return encode_png(width, height, 3, out)


def _data_uri(png_bytes):
    return "data:image/png;base64," + base64.b64encode(png_bytes).decode("ascii")


def _verdict_color(v):
    return {"MATCH": "#2e7d32", "FLIP": "#c98a00", "SUSPECT": "#b00020"}.get(v, "#555")


def write_triptych(html_path, report, rows):
    """Write the self-contained HTML gallery.

    ``report`` is the visual_diff cockpit dict (for the title/summary).
    ``rows`` is a list of dicts: {name, verdict, base_png, cand_png, heat_png,
    detail} where the *_png values are raw PNG bytes (None if unavailable).
    """
    s = report["summary"]
    title = "MC2 Visual Diff -- baseline %s vs candidate %s" % (
        report.get("baseline_commit", "?"),
        report.get("generated_for_commit", "?"),
    )
    parts = [
        "<!doctype html><meta charset='utf-8'>",
        "<title>%s</title>" % title,
        "<style>",
        "body{background:#111;color:#ddd;font-family:Segoe UI,Arial,sans-serif;margin:0;padding:24px}",
        "h1{font-size:18px;margin:0 0 4px}",
        ".sub{color:#999;font-size:13px;margin-bottom:18px}",
        ".tag{display:inline-block;padding:2px 8px;border-radius:3px;color:#fff;font-size:12px;font-weight:600;margin-right:6px}",
        "table{border-collapse:collapse;width:100%}",
        "td,th{border:1px solid #333;padding:8px;vertical-align:top;text-align:left}",
        "th{background:#1c1c1c;font-size:13px}",
        "img{max-width:100%;height:auto;display:block;background:#000;image-rendering:pixelated}",
        ".name{font-weight:600;font-size:14px}",
        ".detail{color:#aaa;font-size:12px;margin-top:6px;white-space:pre-wrap;font-family:Consolas,monospace}",
        ".missing{color:#777;font-style:italic}",
        "</style>",
        "<h1>%s</h1>" % title,
        "<div class='sub'>baseline_dir: %s &nbsp;|&nbsp; candidate_dir: %s<br>"
        % (report["baseline_dir"], report["candidate_dir"]),
        "summary: <span class='tag' style='background:%s'>MATCH %d</span>"
        % (_verdict_color("MATCH"), s["match"]),
        "<span class='tag' style='background:%s'>FLIP %d</span>"
        % (_verdict_color("FLIP"), s["flip"]),
        "<span class='tag' style='background:%s'>SUSPECT %d</span>"
        % (_verdict_color("SUSPECT"), s["suspect"]),
        " &nbsp; exit=%d</div>" % report["exit"],
        "<table><tr><th style='width:18%'>bookmark</th><th>baseline</th>"
        "<th>candidate</th><th>diff heatmap</th></tr>",
    ]
    for r in rows:

        def cell(png):
            if png is None:
                return "<td class='missing'>n/a</td>"
            return "<td><img src='%s'></td>" % _data_uri(png)

        vc = _verdict_color(r["verdict"])
        parts.append(
            "<tr><td><div class='name'>%s</div>"
            "<span class='tag' style='background:%s'>%s</span>"
            "<div class='detail'>%s</div></td>%s%s%s</tr>"
            % (
                r["name"],
                vc,
                r["verdict"],
                r.get("detail", ""),
                cell(r.get("base_png")),
                cell(r.get("cand_png")),
                cell(r.get("heat_png")),
            )
        )
    parts.append("</table>")
    html = "\n".join(parts)
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html)
    return html_path
