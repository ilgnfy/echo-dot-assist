#!/usr/bin/env python3
"""scripts/lib/debloat_edit.py against init-file text shaped like the Echo's (stdlib only; `make unit-debloat_edit`)."""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
import debloat_edit as E  # noqa: E402


def rd(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def wr(path, data, mode="w"):
    with open(path, mode) as f:
        f.write(data)

RC = """\
import init.puffin.rc
import init.perfhald.rc

on early-boot
    start btmac
    start puffin
    start set_memtotal

service sntpd /system/bin/sntp -f
    class late_start
    user root
    group system dbus wifi aipc

service mixer /system/bin/mixer
    class main
    user audio
    group audio dbus aipc media nvram system gadgetsd factory_reset

# a comment at column 0 ends a block too
service puffin /system/bin/PuffinApp /system/etc/AlexaClientSDKConfig.json
   class main
   user puffin
   disabled

on property:com.amazon.puffin.PUFFIN_START=1 && property:audio_mixer.init=true
    start puffin
    start puffinmrmd
    start hassmic
    stop puffin

on post-fs-data
    mkdir /data/ace 0770 ace_group ace_group
    mkdir /data/avs 0750 puffin puffin
    chown cmbd cmbd /data/cmbd/cmbd.db
    # mkdir /data/avs/commented
    chmod 0660 /data/ace/kvstorage/kvsDefault.db
    exec - puffin puffin -- /system/bin/csm2puffin.sh
"""


class Edits(unittest.TestCase):
    def test_services_removed_with_their_options(self):
        out = E.strip_services(RC, ["sntpd", "puffin"])
        self.assertNotIn("service sntpd", out)
        self.assertNotIn("PuffinApp", out)
        self.assertNotIn("user puffin\n", out)
        self.assertNotIn("group system dbus wifi aipc", out)

    def test_other_services_and_stanzas_stay_whole(self):
        out = E.strip_services(RC, ["sntpd", "puffin"])
        self.assertIn("service mixer /system/bin/mixer\n    class main\n    user audio\n    group audio dbus aipc media nvram system gadgetsd factory_reset", out)
        self.assertIn("on early-boot\n    start btmac", out)
        self.assertIn("on post-fs-data\n    mkdir /data/ace", out)
        self.assertIn("# a comment at column 0 ends a block too", out)

    def test_a_service_whose_name_starts_like_a_removed_one_stays(self):
        text = "service puffinmrmd /x\n    class main\nservice puffin /y\n    class main\n"
        self.assertEqual(E.strip_services(text, ["puffin"]), "service puffinmrmd /x\n    class main")

    def test_idempotent(self):
        once = E.strip_services(RC, ["sntpd", "puffin"])
        self.assertEqual(E.strip_services(once, ["sntpd", "puffin"]), once)
        lines = E.strip_lines(RC, "unstart", ["puffin"])
        self.assertEqual(E.strip_lines(lines, "unstart", ["puffin"]), lines)

    def test_unstart_takes_only_exact_names(self):
        out = E.strip_lines(RC, "unstart", ["puffin"])
        self.assertNotIn("    start puffin\n", out)
        self.assertNotIn("    stop puffin", out)
        self.assertIn("    start puffinmrmd", out)           # another service
        self.assertIn("    start hassmic", out)
        self.assertIn("    start btmac", out)

    def test_unimport(self):
        out = E.strip_lines(RC, "unimport", ["init.perfhald.rc"])
        self.assertNotIn("import init.perfhald.rc", out)
        self.assertIn("import init.puffin.rc", out)

    def test_unline_removes_matches_but_not_comments(self):
        out = E.strip_regex(RC, r"^\s*(mkdir|chown|chmod)( -R)? .*(/data/(avs|cmbd)|cmbd\.db)")
        self.assertNotIn("mkdir /data/avs 0750", out)
        self.assertNotIn("chown cmbd", out)
        self.assertIn("# mkdir /data/avs/commented", out)    # comments are left alone
        self.assertIn("mkdir /data/ace 0770", out)           # the key-value store's folder stays
        self.assertIn("chmod 0660 /data/ace/kvstorage/kvsDefault.db", out)

    def test_exec_line(self):
        out = E.strip_regex(RC, r"^\s*exec .*csm2puffin\.sh")
        self.assertNotIn("csm2puffin", out)

    def test_file_without_trailing_newline_and_odd_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a.rc"), os.path.join(d, "b.rc")
            wr(src, b"service x /x\n    class main\n# \xff\xfe odd bytes\non boot\n    start y", "wb")
            subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts/lib/debloat_edit.py"), "services", src, dst, "x"], check=True)
            self.assertEqual(rd(dst, "rb"), b"# \xff\xfe odd bytes\non boot\n    start y")

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a.rc"), os.path.join(d, "b.rc")
            wr(src, RC)
            run = lambda *a: subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts/lib/debloat_edit.py"), *a], capture_output=True, text=True)
            self.assertEqual(run("unstart", src, dst, "puffin").returncode, 0)
            self.assertNotIn("    start puffin\n", rd(dst))
            self.assertEqual(run("unline", src, dst, r"^\s*exec ").returncode, 0)
            self.assertNotIn("csm2puffin", rd(dst))
            self.assertEqual(run("bogus", src, dst, "x").returncode, 2)


if __name__ == "__main__":
    unittest.main()
