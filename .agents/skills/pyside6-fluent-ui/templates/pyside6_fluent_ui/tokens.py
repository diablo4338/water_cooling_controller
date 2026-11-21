"""Load and resolve official Fluent tokens into Qt-facing semantic aliases."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import re
from types import MappingProxyType
from typing import Any, Mapping


class TokenValidationError(ValueError):
    """Raised when token resources are missing, malformed, or inconsistent."""


@dataclass(frozen=True)
class ResolvedTheme:
    """Immutable resolved official and semantic token dictionaries."""

    name: str
    official: Mapping[str, str]
    aliases: Mapping[str, str]
    source_metadata: Mapping[str, Any]
    shell_profile: str | None = None

    def value(self, name: str) -> str:
        """Return a semantic alias or official token value, raising a useful error."""
        if name in self.aliases:
            return self.aliases[name]
        if name in self.official:
            return self.official[name]
        raise TokenValidationError(f"Unknown token or semantic alias: {name!r}")

    def with_alias_overrides(self, overrides: Mapping[str, str], *, name: str | None = None) -> "ResolvedTheme":
        """Return a new theme with semantic overrides, useful for high contrast."""
        aliases = dict(self.aliases)
        aliases.update(overrides)
        return ResolvedTheme(
            name=name or self.name,
            official=self.official,
            aliases=MappingProxyType(aliases),
            source_metadata=self.source_metadata,
            shell_profile=self.shell_profile,
        )


class TokenRepository:
    """Versioned repository of official Fluent themes and Qt/shell alias maps."""

    def __init__(
        self,
        theme_file: str | Path,
        qt_alias_file: str | Path,
        shell_alias_file: str | Path | None = None,
    ) -> None:
        self.theme_file = Path(theme_file)
        self.qt_alias_file = Path(qt_alias_file)
        self.shell_alias_file = Path(shell_alias_file) if shell_alias_file else None

        self._themes = self._load_json(self.theme_file)
        self._qt_map = self._load_json(self.qt_alias_file)
        self._shell_map = self._load_json(self.shell_alias_file) if self.shell_alias_file else None
        self.validate()

    @classmethod
    def from_skill_root(cls, skill_root: str | Path | None = None) -> "TokenRepository":
        """Load resources from this skill/template directory layout."""
        root = Path(skill_root) if skill_root else Path(__file__).resolve().parents[2]
        resources = root / "resources"
        return cls(
            resources / "fluent2-official-web-theme-tokens.json",
            resources / "qt-token-map.json",
            resources / "shell-token-map.json",
        )

    @staticmethod
    def _load_json(path: Path | None) -> dict[str, Any]:
        if path is None:
            return {}
        try:
            raw = path.read_text(encoding="utf-8")
            data = json.loads(raw)
        except FileNotFoundError as exc:
            raise TokenValidationError(f"Token resource not found: {path}") from exc
        except json.JSONDecodeError as exc:
            raise TokenValidationError(f"Invalid JSON in {path}: {exc}") from exc
        if not isinstance(data, dict):
            raise TokenValidationError(f"Expected a JSON object in {path}")
        return data

    @property
    def metadata(self) -> Mapping[str, Any]:
        return MappingProxyType(dict(self._themes.get("metadata", {})))

    def validate(self) -> None:
        """Validate every alias reference against both official themes."""
        expected_count = int(self._themes.get("metadata", {}).get("tokenCountPerTheme", 0) or 0)
        for key in ("webLightTheme", "webDarkTheme"):
            theme = self._themes.get(key)
            if not isinstance(theme, dict) or not theme:
                raise TokenValidationError(f"Missing or empty {key} in {self.theme_file}")
            if expected_count and len(theme) != expected_count:
                raise TokenValidationError(
                    f"{key} contains {len(theme)} tokens; metadata expects {expected_count}"
                )

        qt_specs = self._collect_specs(self._qt_map.get("aliases", {}))
        shell_specs = {}
        if self._shell_map:
            shell_specs.update(self._collect_specs(self._shell_map.get("aliases", {})))
            shell_specs.update(self._collect_specs(self._shell_map.get("metrics", {})))
        all_specs = {**qt_specs, **shell_specs}

        for alias, spec in all_specs.items():
            token = spec.get("token")
            value = spec.get("value")
            if token is None and value is None:
                raise TokenValidationError(f"Alias {alias!r} has neither token nor value")
            if token is not None:
                for theme_name, theme_key in (("light", "webLightTheme"), ("dark", "webDarkTheme")):
                    if token not in self._themes[theme_key]:
                        raise TokenValidationError(
                            f"Alias {alias!r} references unknown token {token!r} in {theme_key}"
                        )
                    documented = spec.get(theme_name)
                    upstream = self._themes[theme_key][token]
                    if documented is not None and str(documented) != str(upstream):
                        raise TokenValidationError(
                            f"Alias {alias!r} has stale {theme_name} value {documented!r}; "
                            f"official token {token!r} resolves to {upstream!r}"
                        )

        if self._shell_map:
            profiles = self._shell_map.get("profiles", {})
            for profile_name, profile in profiles.items():
                overrides = profile.get("status_bar_overrides", {})
                for alias, token in overrides.items():
                    if alias not in shell_specs:
                        raise TokenValidationError(
                            f"Shell profile {profile_name!r} overrides unknown alias {alias!r}"
                        )
                    for theme_key in ("webLightTheme", "webDarkTheme"):
                        if token not in self._themes[theme_key]:
                            raise TokenValidationError(
                                f"Shell profile {profile_name!r} references unknown token {token!r}"
                            )

    @staticmethod
    def _collect_specs(node: Any) -> dict[str, dict[str, Any]]:
        """Flatten nested alias categories while preserving leaf specs."""
        result: dict[str, dict[str, Any]] = {}
        if not isinstance(node, dict):
            return result
        for key, value in node.items():
            if isinstance(value, dict) and ("token" in value or "value" in value):
                result[key] = value
            elif isinstance(value, dict):
                result.update(TokenRepository._collect_specs(value))
        return result

    def resolve(self, theme: str, *, shell_profile: str | None = "fluent-workbench") -> ResolvedTheme:
        """Resolve `light` or `dark` and optionally layer a shell profile."""
        normalized = theme.strip().lower()
        if normalized not in {"light", "dark"}:
            raise TokenValidationError(f"Theme must be 'light' or 'dark', got {theme!r}")

        theme_key = "webLightTheme" if normalized == "light" else "webDarkTheme"
        # Normalize all upstream scalar values to strings. Some official tokens,
        # notably font weights, are emitted as JSON numbers while QSS rendering
        # and Qt-facing consumers expect a uniform string vocabulary.
        official = {key: str(value) for key, value in self._themes[theme_key].items()}
        aliases: dict[str, str] = {}

        for alias, spec in self._collect_specs(self._qt_map.get("aliases", {})).items():
            aliases[alias] = self._resolve_spec(alias, spec, official)

        if shell_profile and self._shell_map:
            shell_specs = {}
            shell_specs.update(self._collect_specs(self._shell_map.get("aliases", {})))
            shell_specs.update(self._collect_specs(self._shell_map.get("metrics", {})))
            for alias, spec in shell_specs.items():
                aliases[alias] = self._resolve_spec(alias, spec, official)
            profile = self._shell_map.get("profiles", {}).get(shell_profile)
            if profile is None:
                raise TokenValidationError(f"Unknown shell profile: {shell_profile!r}")
            for alias, token in profile.get("status_bar_overrides", {}).items():
                aliases[alias] = official[token]

        return ResolvedTheme(
            name=normalized,
            official=MappingProxyType(official),
            aliases=MappingProxyType(aliases),
            source_metadata=self.metadata,
            shell_profile=shell_profile,
        )

    @staticmethod
    def _resolve_spec(alias: str, spec: Mapping[str, Any], official: Mapping[str, str]) -> str:
        if "token" in spec:
            token = str(spec["token"])
            try:
                return official[token]
            except KeyError as exc:
                raise TokenValidationError(f"Alias {alias!r} references missing token {token!r}") from exc
        if "value" in spec:
            return str(spec["value"])
        raise TokenValidationError(f"Alias {alias!r} cannot be resolved")


_PX_RE = re.compile(r"^(-?(?:\d+(?:\.\d+)?|\.\d+))px$")
_MS_RE = re.compile(r"^(\d+(?:\.\d+)?)ms$")
_CUBIC_RE = re.compile(
    r"^cubic-bezier\(\s*([-+]?\d*\.?\d+)\s*,\s*([-+]?\d*\.?\d+)\s*,"
    r"\s*([-+]?\d*\.?\d+)\s*,\s*([-+]?\d*\.?\d+)\s*\)$"
)


def parse_px(value: str) -> float:
    """Parse a CSS logical-pixel value."""
    if value == "0":
        return 0.0
    match = _PX_RE.fullmatch(value.strip())
    if not match:
        raise TokenValidationError(f"Expected px value, got {value!r}")
    return float(match.group(1))


def parse_ms(value: str) -> int:
    """Parse a Fluent duration and round to an integer millisecond."""
    match = _MS_RE.fullmatch(value.strip())
    if not match:
        raise TokenValidationError(f"Expected ms value, got {value!r}")
    return int(round(float(match.group(1))))


def parse_cubic_bezier(value: str) -> tuple[float, float, float, float]:
    """Parse a Fluent cubic-bezier string."""
    match = _CUBIC_RE.fullmatch(value.strip())
    if not match:
        raise TokenValidationError(f"Expected cubic-bezier(...), got {value!r}")
    return tuple(float(match.group(i)) for i in range(1, 5))  # type: ignore[return-value]
