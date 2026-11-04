"""Protected, file-backed users, groups and permission nodes for the NAS API.

Registry stores hashes of randomly generated bearer tokens, not passwords.
Rules are exact nodes, namespace wildcards (asset.*), '*' or explicit
denials prefixed with '-'. Direct user rules win ties against group rules;
more specific rules win first; denial wins ties within a layer.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import tempfile

_IDENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")
_NODE = re.compile(r"[a-z][a-z0-9_-]*(?:\.[a-z0-9_-]+)*(?:\.\*)?\Z")
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
DEFAULT_GROUPS = {
    "admin": ["*"],
    "normal": ["source.read", "product.read", "artifact.read",
               "presence.read", "presence.write"],
}


def validate_name(value):
    if not isinstance(value, str) or not _IDENT.fullmatch(value):
        raise ValueError("invalid user or group name")
    return value


def validate_rule(value):
    raw = value[1:] if isinstance(value, str) and value.startswith("-") else value
    if not isinstance(raw, str) or not (raw == "*" or _NODE.fullmatch(raw)):
        raise ValueError(f"invalid permission rule: {value!r}")
    return value


def match_rule(rule, permission):
    """Minecraft-style prefix wildcard: craft.* includes craft.limestone.*."""
    plain = rule.lstrip("-")
    if plain == "*":
        return True
    if plain.endswith(".*"):
        prefix = plain[:-2]
        return permission == prefix or permission.startswith(prefix + ".")
    return plain == permission


def permitted(rules, permission):
    """Return (matched, allowed, specificity), with deny-wins ties."""
    candidates = []
    for rule in rules:
        if match_rule(rule, permission):
            plain = rule.lstrip("-")
            specificity = 0 if plain == "*" else len(plain[:-2] if plain.endswith(".*") else plain)
            # Exact beats equally long namespace wildcard.
            candidates.append((specificity, 0 if plain.endswith(".*") else 1,
                               not rule.startswith("-")))
    if not candidates:
        return False, False, -1
    best = max((score, exact) for score, exact, _ in candidates)
    return True, all(ok for score, exact, ok in candidates if (score, exact) == best), best[0]


class PermissionRegistry:
    def __init__(self, path):
        self.path = Path(path).expanduser().resolve()
        if not self.path.is_file():
            raise FileNotFoundError(f"permission registry missing: {self.path}; run users init")
        self.load()

    @staticmethod
    def validate(data):
        if not isinstance(data, dict) or data.get("schema_version") != 1:
            raise ValueError("unsupported permission registry format")
        if not isinstance(data.get("groups"), dict) or not isinstance(data.get("users"), dict):
            raise ValueError("registry requires groups and users")
        for name, rules in data["groups"].items():
            validate_name(name)
            if not isinstance(rules, list) or not all(isinstance(r, str) for r in rules):
                raise ValueError("group permissions must be an array")
            for rule in rules:
                validate_rule(rule)
        hashes = set()
        for name, record in data["users"].items():
            validate_name(name)
            if not isinstance(record, dict):
                raise ValueError("invalid user record")
            sha = record.get("token_sha256")
            if not isinstance(sha, str) or not _DIGEST.fullmatch(sha) or sha in hashes:
                raise ValueError("invalid or duplicated token hash")
            hashes.add(sha)
            groups = record.get("groups")
            rules = record.get("permissions")
            if not isinstance(groups, list) or not isinstance(rules, list):
                raise ValueError("user groups and permissions must be arrays")
            for group in groups:
                validate_name(group)
                if group not in data["groups"]:
                    raise ValueError(f"unknown group: {group}")
            for rule in rules:
                validate_rule(rule)
        return data

    def load(self):
        return self.validate(json.loads(self.path.read_text(encoding="utf-8")))

    def identity(self, token):
        if not isinstance(token, str):
            return None
        digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
        data = self.load()  # Revocations and group edits apply to next request.
        found = None
        for username, record in data["users"].items():
            if hmac.compare_digest(digest, record["token_sha256"]):
                found = username
        return found

    def allows(self, username, permission):
        data = self.load()
        record = data["users"].get(username)
        if record is None:
            return False
        validate_rule(permission)
        # Specificity across users/groups: user-specific overrides group
        # at the same specificity, and denial wins conflicting group rules.
        options = []
        user_hit, user_allow, user_rank = permitted(record["permissions"], permission)
        if user_hit:
            options.append((user_rank, 1, user_allow))
        group_rules = [rule for group in record["groups"]
                       for rule in data["groups"][group]]
        group_hit, group_allow, group_rank = permitted(group_rules, permission)
        if group_hit:
            options.append((group_rank, 0, group_allow))
        if not options:
            return False
        highest = max((rank, origin) for rank, origin, _ in options)
        return all(ok for rank, origin, ok in options if (rank, origin) == highest)

    @classmethod
    def initialize(cls, path):
        path = Path(path).expanduser().resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            raise FileExistsError(f"registry already exists: {path}")
        token = secrets.token_urlsafe(40)
        data = {"schema_version": 1, "groups": dict(DEFAULT_GROUPS),
                "users": {"admin": {"groups": ["admin"], "permissions": [],
                                    "token_sha256": hashlib.sha256(token.encode()).hexdigest()}}}
        _atomic_create(path, data)
        return token

    def group_set(self, name, rules):
        validate_name(name)
        rules = [validate_rule(rule) for rule in rules]
        data = self.load()
        data["groups"][name] = rules
        _atomic_replace(self.path, data)

    def user_set(self, name, groups, rules, *, create=False):
        validate_name(name)
        data = self.load()
        for group in groups:
            validate_name(group)
            if group not in data["groups"]:
                raise ValueError(f"unknown group: {group}")
        rules = [validate_rule(rule) for rule in rules]
        record = data["users"].get(name)
        if create and record is not None:
            raise ValueError(f"user already exists: {name}")
        token = None
        if record is None:
            token = secrets.token_urlsafe(40)
            record = {"token_sha256": hashlib.sha256(token.encode()).hexdigest()}
        record.update(groups=list(groups), permissions=list(rules))
        data["users"][name] = record
        _atomic_replace(self.path, data)
        return token

    def rotate_token(self, name):
        data = self.load()
        if name not in data["users"]:
            raise ValueError(f"unknown user: {name}")
        token = secrets.token_urlsafe(40)
        data["users"][name]["token_sha256"] = hashlib.sha256(token.encode()).hexdigest()
        _atomic_replace(self.path, data)
        return token

    def users(self):
        return {name: {"groups": row["groups"], "permissions": row["permissions"]}
                for name, row in self.load()["users"].items()}


def _write_data(path, data, exclusive):
    PermissionRegistry.validate(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".users-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            json.dump(data, out, indent=2, sort_keys=True)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        if os.name != "nt":
            os.chmod(tmp, 0o600)
        if exclusive:
            # Avoid replacing an existing registry during bootstrap.
            with path.open("xb") as output, open(tmp, "rb") as source:
                output.write(source.read())
            if os.name != "nt":
                path.chmod(0o600)
        else:
            os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


def _atomic_create(path, data):
    _write_data(path, data, True)


def _atomic_replace(path, data):
    _write_data(path, data, False)
