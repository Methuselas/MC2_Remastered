import sys, json, os, shutil

d = json.load(sys.stdin)
f = d.get('tool_input', {}).get('file_path', '').replace(chr(92), '/')
ext = f.rsplit('.', 1)[-1] if '.' in f else ''

if 'nifty-mendeleev/shaders/' not in f:
    sys.exit(0)
# Include .hglsl: shader includes (e.g. view_uniforms.hglsl) are loaded at
# runtime and MUST be deployed too — missing them caused the MECH-VIEWUNIFORMS
# deploy-gap (see docs/mech-viewuniforms-source-dump.md).
if ext not in ('frag', 'vert', 'tesc', 'tese', 'geom', 'hglsl', 'glsl', 'comp'):
    sys.exit(0)

# Deploy target MUST match the live runtime dir (currently v0.4). v0.1.1 was
# stale — deploying there silently did nothing for the running game.
dst_base = 'A:/Games/mc2-opengl/mc2-win64-v0.4/shaders'
dst_dir = dst_base + '/include' if '/shaders/include/' in f else dst_base
dst = dst_dir + '/' + os.path.basename(f)

shutil.copy2(f, dst)
print('[shader-hook] deployed ' + os.path.basename(f) + ' -> ' + dst_dir)
