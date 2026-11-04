"""GPU-free tests for first-run project setup orchestration."""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import setup_wizard


class SetupWizardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "Demo"
        self.exe = self.root / "Project.exe"
        self.exe.write_bytes(b"native CLI placeholder")

    def fake_project(self, name, destination, project_exe=None):
        target = Path(destination)
        target.mkdir(parents=True, exist_ok=True)
        (target / "content").mkdir(exist_ok=True)
        (target / "project.tourproj").write_text("guid=native-owned\n")
        return target

    def test_project_only_does_not_install_asset_services(self):
        with patch.object(setup_wizard, "create_project", self.fake_project), \
                patch.object(setup_wizard, "configure_host") as host:
            result = setup_wizard.setup("Demo", self.project, "none",
                                        project_exe=self.exe)
        self.assertEqual(result["mode"], "none")
        host.assert_not_called()
        self.assertTrue((self.project / "project.tourproj").is_file())

    def test_remote_requires_https_and_validates_before_creating_project(self):
        with patch.object(setup_wizard, "create_project") as creator:
            with self.assertRaisesRegex(ValueError, "HTTPS"):
                setup_wizard.setup("Demo", self.project, "remote",
                                   url="http://nas.local:8765", token="x"*32)
            creator.assert_not_called()
        with patch.object(setup_wizard, "validate_remote", return_value=2), \
                patch.object(setup_wizard, "create_project", self.fake_project), \
                patch.object(setup_wizard, "configure_client", return_value=self.root/"client.json") as config:
            result = setup_wizard.setup("Demo", self.project, "remote",
                                        url="https://assets.example.lan",
                                        token="secure-individual-user-token-123456")
        self.assertEqual(result["mode"], "remote")
        self.assertTrue(config.called)
        self.assertFalse((self.project / "server.json").exists())

    def test_failed_remote_validation_does_not_create_project(self):
        with patch.object(setup_wizard, "validate_remote", side_effect=OSError("offline")), \
                patch.object(setup_wizard, "create_project") as creator:
            with self.assertRaisesRegex(OSError, "offline"):
                setup_wizard.setup("Demo", self.project, "remote",
                                   url="https://nas.example", token="x"*32)
            creator.assert_not_called()

    def test_local_creates_host_and_client_configuration(self):
        with patch.object(setup_wizard, "create_project", self.fake_project), \
                patch.object(setup_wizard, "configure_host",
                             return_value=(self.root/"history"/"server.json", "local-token")) as host, \
                patch.object(setup_wizard, "configure_client",
                             return_value=self.root/"client.json") as client, \
                patch.object(setup_wizard, "run_detached") as run:
            result = setup_wizard.setup("Demo", self.project, "local",
                                        project_exe=self.exe, port=8877)
        self.assertEqual(result["endpoint"], "http://127.0.0.1:8877")
        self.assertTrue(result["started"])
        self.assertEqual(host.call_args.kwargs["port"], 8877)
        self.assertEqual(client.call_args.args[1], "local-token")
        run.assert_called_once()


if __name__ == "__main__":
    unittest.main()
