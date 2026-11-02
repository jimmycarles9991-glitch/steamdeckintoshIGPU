#!/usr/bin/env python3
"""Offline test for tools/linux_hubp_dump.py using a fake header, sysfs and amdgpu_regs."""
import json, os, struct, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import linux_hubp_dump as t

HDR = """
#define mmHUBP0_DCSURF_SURFACE_CONFIG 0x05e5
#define mmHUBP0_DCSURF_SURFACE_CONFIG_BASE_IDX 2
#define mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION 0x05e8
#define mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_BASE_IDX 2
#define mmHUBP0_DCHUBP_CNTL_STATUS 0x05f0
#define mmHUBP0_DCHUBP_CNTL_STATUS_BASE_IDX 2
#define mmHUBPREQ3_DCSURF_FLIP_CONTROL 0x08af
#define mmHUBPREQ3_DCSURF_FLIP_CONTROL_BASE_IDX 2
#define mmOTG0_OTG_CONTROL 0x1b41
#define mmOTG0_OTG_CONTROL_BASE_IDX 2
"""
BASE = 0x34C0

def main():
    r = t.collect_registers(HDR)
    assert set(r[0]) == {"DCSURF_SURFACE_CONFIG", "DCSURF_PRI_VIEWPORT_DIMENSION"}, r[0]   # status + OTG skipped
    assert r[3] == {"DCSURF_FLIP_CONTROL": (0x08AF, 2)}, r[3]
    with tempfile.TemporaryDirectory() as d:
        dev = os.path.join(d, "dev"); os.makedirs(os.path.join(dev, "ip_discovery/die/0/271/0"))
        open(os.path.join(dev, "ip_discovery/die/0/271/0/base_addr"), "w").write(f"0x12 0xC0 0x{BASE:x}\n")
        assert t.ip_bases(dev) == [0x12, 0xC0, BASE]
        def regs_file(cfg, vp, name):
            buf = bytearray(0x20000 * 4)
            def put(dw, v): struct.pack_into("<I", buf, (BASE + dw) * 4, v)
            put(0x05e5, cfg); put(0x05e8, vp); put(0x08af, 0x100)
            p = os.path.join(d, name); open(p, "wb").write(buf); return p
        header = os.path.join(d, "h.h"); open(header, "w").write(HDR)
        import types
        def run(label, cfg, vp):
            out = os.path.join(d, label + ".json")
            t.do_dump(types.SimpleNamespace(bdf="x", regs=regs_file(cfg, vp, label + ".bin"), header=header,
                                            fetch=False, label=label, output=out))
            # do_dump looks the device up by bdf under /sys; patch ip_bases for the fake tree
            return out
        orig = t.ip_bases; t.ip_bases = lambda _dev: [0x12, 0xC0, BASE]
        try:
            a = run("rot0", 0x008, (1280 << 16) | 800)
            b = run("rot270", 0x308, (800 << 16) | 1280)
        finally:
            t.ip_bases = orig
        A = json.load(open(a)); assert A["hubp"]["0"]["DCSURF_SURFACE_CONFIG"] == 0x008
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            t.do_diff(types.SimpleNamespace(a=a, b=b))
        out = buf.getvalue()
        assert "ROTATION_ANGLE=0" in out and "ROTATION_ANGLE=3" in out, out
        assert "WIDTH=800" in out and "WIDTH=1280" in out, out
        assert "FLIP_CONTROL" not in out                                     # unchanged registers not listed
        assert "2 differing register(s) total" in out, out
    print("test_hubp_dump: ok")

main()
