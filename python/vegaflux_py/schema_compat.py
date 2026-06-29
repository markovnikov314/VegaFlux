from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from dataclasses import field
from pathlib import Path


_FIELD_RE = re.compile(
    r"^(?:(optional|repeated)\s+)?(map<[^>]+>|[A-Za-z_][\w.]*)\s+([A-Za-z_]\w*)\s*=\s*(\d+)\b(?P<options>\s*\[[^\]]+\])?"
)
_MESSAGE_RE = re.compile(r"^\s*message\s+([A-Za-z_]\w*)\s*\{")
_ENUM_RE = re.compile(r"^\s*enum\s+([A-Za-z_]\w*)\s*\{")
_ENUM_VALUE_RE = re.compile(r"^([A-Za-z_]\w*)\s*=\s*(-?\d+)\b")
_SYNTAX_RE = re.compile(r'^syntax\s*=\s*"([^"]+)"\s*;')
_PACKAGE_RE = re.compile(r"^package\s+([A-Za-z_][\w.]*)\s*;")
_OPTION_RE = re.compile(r"^option\s+([A-Za-z_][\w.]*)\s*=\s*(.+);")
_RESERVED_RE = re.compile(r"^reserved\s+(.+);")


@dataclass(frozen=True)
class Field:
    name: str
    type_name: str
    label: str
    number: int
    deprecated: bool = False


@dataclass(frozen=True)
class EnumValue:
    name: str
    number: int


@dataclass
class ProtoSpec:
    syntax: str = ""
    package: str = ""
    options: dict[str, str] = field(default_factory=dict)
    messages: dict[str, dict[int, Field]] = field(default_factory=dict)
    message_names: dict[str, dict[str, Field]] = field(default_factory=dict)
    enums: dict[str, dict[int, EnumValue]] = field(default_factory=dict)
    enum_names: dict[str, dict[str, EnumValue]] = field(default_factory=dict)
    reserved_numbers: dict[str, set[int]] = field(default_factory=dict)
    reserved_names: dict[str, set[str]] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)


def scoped_name(stack: list[tuple[str, str]], name: str) -> str:
    parent_messages = [item[1] for item in stack if item[0] == "message"]
    return f"{parent_messages[-1]}.{name}" if parent_messages else name


def option_context(stack: list[tuple[str, str]]) -> str:
    return f"{stack[-1][0]}:{stack[-1][1]}" if stack else "file"


def deprecated_option(raw_options: str | None) -> bool:
    return bool(raw_options and re.search(r"\bdeprecated\s*=\s*true\b", raw_options))


def parse_reserved(line: str) -> tuple[set[int], set[str]]:
    numbers: set[int] = set()
    names: set[str] = set()
    match = _RESERVED_RE.match(line)
    if not match:
        return numbers, names
    for item in match.group(1).split(","):
        token = item.strip()
        if token.startswith('"') and token.endswith('"'):
            names.add(token.strip('"'))
        elif " to " in token:
            start_raw, end_raw = [part.strip() for part in token.split(" to ", 1)]
            if start_raw.isdigit() and end_raw.isdigit():
                numbers.update(range(int(start_raw), int(end_raw) + 1))
        elif token.isdigit():
            numbers.add(int(token))
    return numbers, names


def parse_proto(path: Path) -> ProtoSpec:
    spec = ProtoSpec()
    stack: list[tuple[str, str]] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line:
            continue
        syntax_match = _SYNTAX_RE.match(line)
        package_match = _PACKAGE_RE.match(line)
        option_match = _OPTION_RE.match(line)
        if syntax_match:
            spec.syntax = syntax_match.group(1)
            continue
        if package_match:
            spec.package = package_match.group(1)
            continue
        if option_match:
            spec.options[f"{option_context(stack)}.{option_match.group(1)}"] = option_match.group(2).strip()
            continue
        message_match = _MESSAGE_RE.match(line)
        enum_match = _ENUM_RE.match(line)
        if message_match:
            name = scoped_name(stack, message_match.group(1))
            stack.append(("message", name))
            spec.messages.setdefault(name, {})
            spec.message_names.setdefault(name, {})
            continue
        if enum_match:
            name = scoped_name(stack, enum_match.group(1))
            stack.append(("enum", name))
            spec.enums.setdefault(name, {})
            spec.enum_names.setdefault(name, {})
            continue
        if stack and stack[-1][0] == "message":
            reserved_numbers, reserved_names = parse_reserved(line)
            if reserved_numbers or reserved_names:
                message_name = stack[-1][1]
                spec.reserved_numbers.setdefault(message_name, set()).update(reserved_numbers)
                spec.reserved_names.setdefault(message_name, set()).update(reserved_names)
                continue
            field_match = _FIELD_RE.match(line)
            if field_match:
                label = field_match.group(1) or ""
                type_name = field_match.group(2)
                name = field_match.group(3)
                number = int(field_match.group(4))
                field_value = Field(name, type_name, label, number, deprecated_option(field_match.group("options")))
                message_name = stack[-1][1]
                fields_by_number = spec.messages.setdefault(message_name, {})
                fields_by_name = spec.message_names.setdefault(message_name, {})
                if number in fields_by_number:
                    spec.errors.append(
                        f"duplicate field number in current schema: {message_name}.{number} "
                        f"({fields_by_number[number].name}, {name})"
                    )
                else:
                    fields_by_number[number] = field_value
                if name in fields_by_name:
                    spec.errors.append(f"duplicate field name in current schema: {message_name}.{name}")
                else:
                    fields_by_name[name] = field_value
        if stack and stack[-1][0] == "enum":
            enum_match = _ENUM_VALUE_RE.match(line)
            if enum_match:
                enum_name = stack[-1][1]
                name = enum_match.group(1)
                number = int(enum_match.group(2))
                enum_value = EnumValue(name, number)
                values_by_number = spec.enums.setdefault(enum_name, {})
                values_by_name = spec.enum_names.setdefault(enum_name, {})
                if number in values_by_number:
                    spec.errors.append(
                        f"duplicate enum number in current schema: {enum_name}.{number} "
                        f"({values_by_number[number].name}, {name})"
                    )
                else:
                    values_by_number[number] = enum_value
                if name in values_by_name:
                    spec.errors.append(f"duplicate enum name in current schema: {enum_name}.{name}")
                else:
                    values_by_name[name] = enum_value
        for _ in range(line.count("}")):
            if stack:
                stack.pop()
    return spec


def compatibility_errors(baseline: Path, current: Path) -> list[str]:
    old = parse_proto(baseline)
    new = parse_proto(current)
    errors: list[str] = [f"baseline parse error: {error}" for error in old.errors]
    errors.extend(new.errors)
    if old.syntax != new.syntax:
        errors.append(f"syntax changed: {old.syntax}->{new.syntax}")
    if old.package != new.package:
        errors.append(f"package changed: {old.package}->{new.package}")
    if old.options != new.options:
        errors.append("proto options changed")
    for message_name, old_fields in old.messages.items():
        if message_name not in new.messages:
            errors.append(f"message removed: {message_name}")
            continue
        new_by_number = new.messages[message_name]
        new_by_name = {field.name: field for field in new_by_number.values()}
        reserved_numbers = new.reserved_numbers.get(message_name, set())
        reserved_names = new.reserved_names.get(message_name, set())
        for number, field_value in new_by_number.items():
            if number in reserved_numbers:
                errors.append(f"active field uses reserved number: {message_name}.{field_value.name} = {number}")
            if field_value.name in reserved_names:
                errors.append(f"active field uses reserved name: {message_name}.{field_value.name}")
        for number, old_field in old_fields.items():
            new_field = new_by_number.get(number)
            if new_field is None:
                moved = new_by_name.get(old_field.name)
                if moved:
                    errors.append(f"field number changed: {message_name}.{old_field.name} {number}->{moved.number}")
                else:
                    errors.append(f"field removed: {message_name}.{old_field.name} = {number}; reserve only after ADR")
                continue
            if new_field.name != old_field.name:
                errors.append(
                    f"field number reused: {message_name}.{number} was {old_field.name}, now {new_field.name}"
                )
            if (new_field.type_name, new_field.label) != (old_field.type_name, old_field.label):
                errors.append(f"field type changed: {message_name}.{old_field.name} = {number}")
            if new_field.deprecated != old_field.deprecated:
                errors.append(f"field deprecation changed: {message_name}.{old_field.name} = {number}; requires ADR")
            renamed = new_by_name.get(old_field.name)
            if renamed and renamed.number != number:
                errors.append(f"field number changed: {message_name}.{old_field.name} {number}->{renamed.number}")
    for enum_name, old_values in old.enums.items():
        if enum_name not in new.enums:
            errors.append(f"enum removed: {enum_name}")
            continue
        new_by_number = new.enums[enum_name]
        new_by_name = {value.name: value for value in new_by_number.values()}
        for number, old_value in old_values.items():
            new_value = new_by_number.get(number)
            if new_value is None:
                moved = new_by_name.get(old_value.name)
                if moved:
                    errors.append(f"enum number changed: {enum_name}.{old_value.name} {number}->{moved.number}")
                else:
                    errors.append(f"enum value removed: {enum_name}.{old_value.name} = {number}")
                continue
            if new_value.name != old_value.name:
                errors.append(f"enum number reused: {enum_name}.{number} was {old_value.name}, now {new_value.name}")
            renamed = new_by_name.get(old_value.name)
            if renamed and renamed.number != number:
                errors.append(f"enum number changed: {enum_name}.{old_value.name} {number}->{renamed.number}")
    return errors


def generated_code_check(paths: list[Path]) -> dict:
    protoc = shutil.which("protoc")
    if protoc is None:
        return {"status": "skipped", "reason": "protoc unavailable"}
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as temp_dir:
        output_root = Path(temp_dir)
        for path in paths:
            proto_path = path.resolve()
            out_dir = output_root / proto_path.stem
            out_dir.mkdir()
            completed = subprocess.run(
                [protoc, f"--proto_path={proto_path.parent}", f"--python_out={out_dir}", str(proto_path)],
                cwd=proto_path.parent,
                text=True,
                capture_output=True,
                check=False,
            )
            if completed.returncode != 0:
                failures.append(f"{path}: {(completed.stderr or completed.stdout).strip()}")
    if failures:
        return {"status": "fail", "errors": failures}
    return {"status": "pass", "tool": protoc}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--skip-generated-code-check", action="store_true")
    args = parser.parse_args(argv)

    errors = compatibility_errors(args.baseline, args.current)
    generated = {"status": "not_run"}
    if not args.skip_generated_code_check:
        generated = generated_code_check([args.baseline, args.current])
        if generated["status"] == "fail":
            errors.extend(generated["errors"])
    policy_notes = [
        "Do not remove, renumber, rename, or deprecate canonical fields without ADR and migration tests.",
        "Removed fields must reserve both number and name in the same schema migration.",
        "Generated-code smoke is optional when protoc is unavailable.",
    ]
    if errors:
        print(
            json.dumps(
                {"status": "fail", "errors": errors, "generated_code_check": generated, "policy_notes": policy_notes},
                indent=2,
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return 1
    print(
        json.dumps(
            {
                "status": "pass",
                "baseline": str(args.baseline),
                "current": str(args.current),
                "generated_code_check": generated,
                "policy_notes": policy_notes,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
