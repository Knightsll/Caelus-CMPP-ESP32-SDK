# Install in Codex and Hermes

This directory follows the open Agent Skills layout: one directory containing
`SKILL.md`, `scripts/`, `references/`, and optional agent metadata. Codex and
Hermes use the same files; do not maintain platform-specific forks.

## Codex

Install the repository subdirectory with the Codex skill installer, or copy the
complete directory to:

```text
~/.codex/skills/caelus-pair-device/
```

When using a checkout for development, a symlink from that location to
`<repo>/skills/caelus-pair-device` keeps updates in one place. Restart or reload
the Codex session after installation if the current session does not yet list
the Skill.

## Hermes

Hermes natively supports `SKILL.md` and GitHub repository paths. Install the
repository subdirectory with:

```bash
hermes skills install <owner>/Caelus-CMPP-ESP32-SDK/skills/caelus-pair-device
```

Alternatively copy the directory to:

```text
~/.hermes/skills/caelus-pair-device/
```

For a shared checkout, add its parent `skills` directory under
`skills.external_dirs` in `~/.hermes/config.yaml`:

```yaml
skills:
  external_dirs:
    - /absolute/path/to/Caelus-CMPP-ESP32-SDK/skills
```

Hermes automatically exposes an installed Skill as `/caelus-pair-device`.

## One-time BLE runtime

After either installation, authorize the isolated dependency setup once:

```bash
python3 <skill-dir>/scripts/setup_ble.py --yes
```

The environment is created under
`~/.cache/caelus-pair-device/venv` by default. Override it with
`CAELUS_BLE_VENV`. The Skill launcher automatically re-executes inside that
environment, so Codex and Hermes call the same command afterward.
