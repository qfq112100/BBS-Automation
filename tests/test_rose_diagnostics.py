"""Inert standard-library tests. Only unique temporary fixtures are created."""
import datetime as dt
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('rose_diagnostics', Path(__file__).parents[1] / 'scripts' / 'collect_rose_diagnostics.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
HOOK = 'INFO game found; hook installed tid=1 pid=2\nstatus injected dll attached\n'

class DiagnosticsTests(unittest.TestCase):
    def test_attached_is_not_success(self):
        r = m.classify(HOOK)
        self.assertEqual(r['state'], 'attached_only')
        self.assertFalse(r['full_goal_verified'])
    def test_actual_vrf_refusal_wins(self):
        r = m.classify(HOOK + 'dll ERROR host verification failed, refusing overlay: [VRF00000008]\nstatus injecting scanning for game\ndll detached')
        self.assertEqual(r['state'], 'overlay_rejected')
        self.assertTrue(r['session_ended'])
    def test_refusal_overrides_positive_markers(self):
        r = m.classify(HOOK + 'overlay verified\nredirected a.wad.client\nERROR refusing overlay')
        self.assertEqual(r['state'], 'overlay_rejected')
    def test_hook_failure(self):
        self.assertEqual(m.classify('hook install failed\nSetWindowsHookEx failed:0')['state'], 'hook_failed')
    def test_verified_without_wad_is_not_success(self):
        self.assertEqual(m.classify(HOOK + 'overlay verified')['state'], 'verified_without_redirect_evidence')
    def test_zero_files_is_not_redirect_evidence(self):
        r = m.classify(HOOK + 'overlay verified\nredirected 0 files')
        self.assertFalse(r['wad_redirect_marker'])
    def test_redirect_without_verified_is_not_success(self):
        self.assertEqual(m.classify(HOOK + 'redirected a.wad.client')['state'], 'redirects_without_complete_verification')
    def test_positive_markers_still_need_visual_confirmation(self):
        r = m.classify(HOOK + 'overlay verified\nredirected a.wad.client')
        self.assertEqual(r['state'], 'overlay_evidence_present')
        self.assertFalse(r['human_visual_confirmed'])
        self.assertFalse(r['full_goal_verified'])
    def test_latest_game_ignores_older_rejection(self):
        self.assertEqual(m.classify(HOOK + 'ERROR refusing overlay\n' + HOOK + 'overlay verified\nredirected b.wad')['state'], 'overlay_evidence_present')
    def test_latest_game_failure_overrides_old_success(self):
        self.assertEqual(m.classify(HOOK + 'overlay verified\nredirected a.wad\n' + HOOK + 'ERROR refusing overlay')['state'], 'overlay_rejected')
    def test_other_dll_error_prevents_positive_classification(self):
        self.assertEqual(m.classify(HOOK + 'overlay verified\nredirected a.wad\nERROR failed reading data')['state'], 'session_error')
    def test_truncated_log_is_not_complete_evidence(self):
        self.assertEqual(m.classify(HOOK + 'overlay verified\nredirected a.wad', True)['state'], 'partial_log_manual_review')
    def test_missing_logs_does_not_create_directories(self):
        with tempfile.TemporaryDirectory(prefix='bbs-diagnostics-') as root:
            path = Path(root) / 'missing'
            self.assertEqual(m.collect(path, path)['state'], 'no_session')
            self.assertFalse(path.exists())
    def test_historical_cutoff_and_no_modification(self):
        with tempfile.TemporaryDirectory(prefix='bbs-diagnostics-') as root:
            path = Path(root)
            log = path / 'rose_runoverlay_24-10-2026_18-20-16.log'
            log.write_text(HOOK + 'ERROR refusing overlay', encoding='utf-8')
            before = log.read_bytes()
            cutoff = dt.datetime.fromisoformat('2026-10-24T19:23:00+08:00')
            self.assertEqual(m.collect(path, path, cutoff)['state'], 'historical_only')
            self.assertEqual(log.read_bytes(), before)
    def test_filename_not_copy_mtime_selects_latest(self):
        with tempfile.TemporaryDirectory(prefix='bbs-diagnostics-') as root:
            path = Path(root)
            (path / 'rose_runoverlay_24-10-2026_20-00-00.log').write_text(HOOK, encoding='utf-8')
            (path / 'rose_runoverlay_24-10-2026_18-00-00.log').write_text(HOOK + 'ERROR refusing overlay', encoding='utf-8')
            self.assertEqual(m.collect(path, path)['state'], 'attached_only')
    def test_hash_check_does_not_rewrite_fixture(self):
        with tempfile.TemporaryDirectory(prefix='bbs-diagnostics-') as root:
            path = Path(root)
            p = path / 'fixture.bin'
            p.write_bytes(b'fixture')
            before = p.read_bytes()
            pin = hashlib.sha256(before).hexdigest()
            self.assertTrue(m.runtime_status(path, {'fixture.bin': pin})['all_match'])
            self.assertFalse(m.runtime_status(path, {'fixture.bin': '0' * 64})['all_match'])
            self.assertEqual(p.read_bytes(), before)

if __name__ == '__main__':
    unittest.main(verbosity=2)
