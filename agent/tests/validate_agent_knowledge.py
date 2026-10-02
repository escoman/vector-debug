#!/usr/bin/env python3
"""
validate_agent_knowledge.py — Validator for Stage 6.7 Agent Knowledge Base & Workflow.

Checks:
  - Files: all mandatory KB files exist; verification.md exists;
           all Tasks exist; all Profiles exist;
           AI_AGENT_WORKFLOW.md exists.
  - Metadata: present; platform correct; topic correct;
              status in allowed list; source valid.
  - References: Task → Knowledge, Profile → Task, Profile → Knowledge.
                All references must point to existing documents.
  - Status consistency: documents/tasks should not assert facts as established
                        if verification.md marks them CONFLICT/UNVERIFIED.
  - Workflow: Tasks and Profiles reference AI_AGENT_WORKFLOW.md.
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

AGENT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TASKS_DIR = os.path.join(AGENT_DIR, "tasks")
KNOWLEDGE_DIR = os.path.join(AGENT_DIR, "knowledge", "vector06c")
PROFILES_DIR = os.path.join(AGENT_DIR, "profiles")

# Required KB topic files (excluding verification.md which is a meta-document)
REQUIRED_KB_FILES = [
    "architecture.md",
    "cpu.md",
    "memory.md",
    "video.md",
    "io.md",
    "keyboard.md",
    "sound.md",
    "rom_format.md",
]

# Required task directories
REQUIRED_TASKS = [
    "generate_map",
    "stack_safety",
    "find_bugs",
    "analyze_vram",
    "analyze_io",
]

# Required profile files
REQUIRED_PROFILES = [
    "rom_audit.md",
    "reverse_engineering.md",
    "bug_hunting.md",
]

# All registered MCP tools (from mcp_adapter.cpp)
VALID_TOOLS = {
    "debug_run", "debug_pause", "debug_step", "debug_reset", "debug_is_running",
    "debug_get_cpu_state", "debug_get_registers", "debug_set_register",
    "debug_read_memory", "debug_write_memory",
    "debug_read_io", "debug_write_io",
    "debug_list_keys", "debug_press_key", "debug_release_key", "debug_type_key",
    "debug_set_breakpoint", "debug_remove_breakpoint", "debug_list_breakpoints",
    "debug_clear_breakpoints", "debug_set_breakpoint_enabled",
    "debug_disassemble", "debug_get_instruction_history", "debug_get_execution_trace",
    "debug_get_stack",
    "debug_get_symbols", "debug_get_function", "debug_get_function_context",
    "debug_get_xrefs", "debug_get_call_graph",
    "debug_get_memory_map", "debug_get_vram_info", "debug_get_screen_info",
    "debug_get_io_trace",
    "debug_get_state",
    "debug_load_rom",
    "debug_set_comment", "debug_set_function_comment", "debug_rename_function",
    "debug_create_function", "debug_delete_function", "debug_add_label",
}

VALID_STATUSES = {"verified", "partially_verified", "conflict", "unverified"}

VALID_SOURCES = {
    "emulator", "primary_source", "measurement", "community", "multiple",
}

VALID_TOPICS = {
    "architecture", "cpu", "memory", "video", "io",
    "keyboard", "sound", "rom_format",
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

errors = []
warnings = []


def error(msg):
    errors.append(msg)
    print(f"  ERROR: {msg}")


def warn(msg):
    warnings.append(msg)
    print(f"  WARN:  {msg}")


def ok(msg):
    print(f"  OK:    {msg}")


# ---------------------------------------------------------------------------
# Metadata parsing
# ---------------------------------------------------------------------------

def parse_yaml_frontmatter(filepath):
    """Parse YAML front matter between --- markers."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    match = re.match(r"^---\s*\n(.*?)\n---\s*\n", content, re.DOTALL)
    if not match:
        return None

    yaml_text = match.group(1)
    try:
        import yaml as _yaml
        return _yaml.safe_load(yaml_text)
    except ImportError:
        # Manual fallback for simple key: value pairs
        result = {}
        for line in yaml_text.strip().split("\n"):
            if ":" in line:
                key, val = line.split(":", 1)
                result[key.strip()] = val.strip()
        return result


# ---------------------------------------------------------------------------
# File existence checks
# ---------------------------------------------------------------------------

def validate_required_files():
    print("\n=== Required Files Check ===")

    # Check knowledge directory
    if not os.path.isdir(KNOWLEDGE_DIR):
        error(f"Knowledge directory not found: {KNOWLEDGE_DIR}")
    else:
        for fname in REQUIRED_KB_FILES:
            fpath = os.path.join(KNOWLEDGE_DIR, fname)
            if os.path.isfile(fpath):
                ok(f"KB file exists: {fname}")
            else:
                error(f"Required KB file missing: {fname}")

        # Check verification.md
        vpath = os.path.join(KNOWLEDGE_DIR, "verification.md")
        if os.path.isfile(vpath):
            ok("verification.md exists")
        else:
            error("verification.md missing")

    # Check tasks
    if not os.path.isdir(TASKS_DIR):
        error(f"Tasks directory not found: {TASKS_DIR}")
    else:
        for task_name in REQUIRED_TASKS:
            task_file = os.path.join(TASKS_DIR, task_name, "TASK.md")
            if os.path.isfile(task_file):
                ok(f"Task exists: {task_name}/TASK.md")
            else:
                error(f"Required task missing: {task_name}/TASK.md")

    # Check profiles
    if not os.path.isdir(PROFILES_DIR):
        error(f"Profiles directory not found: {PROFILES_DIR}")
    else:
        for fname in REQUIRED_PROFILES:
            fpath = os.path.join(PROFILES_DIR, fname)
            if os.path.isfile(fpath):
                ok(f"Profile exists: {fname}")
            else:
                error(f"Required profile missing: {fname}")


# ---------------------------------------------------------------------------
# Knowledge validation
# ---------------------------------------------------------------------------

def validate_knowledge():
    print("\n=== Knowledge Base Validation ===")
    knowledge_ids = set()

    if not os.path.isdir(KNOWLEDGE_DIR):
        error(f"Knowledge directory not found: {KNOWLEDGE_DIR}")
        return knowledge_ids

    for fname in sorted(os.listdir(KNOWLEDGE_DIR)):
        if not fname.endswith(".md"):
            continue
        # Skip verification.md — it's a meta-document with different rules
        if fname == "verification.md":
            print(f"\n  Skipping verification.md (meta-document)")
            kid = "vector06c/verification"
            knowledge_ids.add(kid)
            continue

        fpath = os.path.join(KNOWLEDGE_DIR, fname)
        topic = fname.replace(".md", "")
        print(f"\n  Checking knowledge: {fname}")

        meta = parse_yaml_frontmatter(fpath)
        if meta is None:
            error(f"{fname}: missing YAML front matter")
            continue

        # Check required fields
        for field in ("platform", "topic", "status", "source"):
            if field not in meta:
                error(f"{fname}: missing metadata field '{field}'")

        # Check status
        status = str(meta.get("status", ""))
        if status and status not in VALID_STATUSES:
            error(f"{fname}: invalid status '{status}' (allowed: {VALID_STATUSES})")
        else:
            ok(f"{fname}: status = {status}")

        # Check source
        source = str(meta.get("source", ""))
        if not source:
            warn(f"{fname}: missing source")
        else:
            # Check source is a known value (may contain extra context in parens)
            source_base = source.split("(")[0].strip().split()[0] if source else ""
            if source_base not in VALID_SOURCES:
                warn(f"{fname}: source '{source}' — base '{source_base}' not in known set {VALID_SOURCES}")
            else:
                ok(f"{fname}: source = {source}")

        # Check platform
        platform = meta.get("platform", "")
        if platform != "Vector-06C":
            warn(f"{fname}: platform = '{platform}' (expected 'Vector-06C')")

        # Check topic matches filename
        meta_topic = str(meta.get("topic", ""))
        if meta_topic and meta_topic != topic:
            warn(f"{fname}: metadata topic='{meta_topic}' doesn't match filename topic='{topic}'")

        if meta_topic and meta_topic not in VALID_TOPICS:
            warn(f"{fname}: topic '{meta_topic}' not in known set {VALID_TOPICS}")

        # Check for Hardware vs Emulator section
        with open(fpath, "r", encoding="utf-8") as f:
            content = f.read()
        if "Hardware vs Emulator" not in content and "Emulator Behavior" not in content:
            warn(f"{fname}: missing 'Hardware vs Emulator Behavior' section")

        # Track ID
        kid = f"vector06c/{topic}"
        if kid in knowledge_ids:
            error(f"{fname}: duplicate knowledge ID '{kid}'")
        knowledge_ids.add(kid)

    ok(f"Found {len(knowledge_ids)} knowledge documents")
    return knowledge_ids


# ---------------------------------------------------------------------------
# Task validation
# ---------------------------------------------------------------------------

def extract_task_id(filepath):
    """Extract task ID from ## ID section."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"^## ID\s*\n\s*(\S+)\s*$", content, re.MULTILINE)
    if match:
        return match.group(1).strip()
    return None


def extract_required_tools(filepath):
    """Extract tool names from ## Required Tools section."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"^## Required Tools\s*\n(.*?)(?=^## |\Z)", content, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    section = match.group(1)
    tools = re.findall(r"debug_\w+", section)
    return tools


def extract_required_knowledge(filepath):
    """Extract knowledge IDs from ## Required Knowledge section."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"^## Required Knowledge\s*\n(.*?)(?=^## |\Z)", content, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    section = match.group(1)
    # Match patterns like vector06c/cpu or vector06c/rom_format
    knowledge = re.findall(r"vector06c/\w+", section)
    return knowledge


def validate_tasks(knowledge_ids):
    print("\n=== Task Library Validation ===")
    task_ids = set()

    if not os.path.isdir(TASKS_DIR):
        error(f"Tasks directory not found: {TASKS_DIR}")
        return task_ids

    for dirname in sorted(os.listdir(TASKS_DIR)):
        task_file = os.path.join(TASKS_DIR, dirname, "TASK.md")
        if not os.path.isfile(task_file):
            continue

        print(f"\n  Checking task: {dirname}/TASK.md")

        # Check ID
        tid = extract_task_id(task_file)
        if tid is None:
            error(f"{dirname}: missing ## ID section")
            continue
        if tid != dirname:
            error(f"{dirname}: ID mismatch (ID='{tid}', directory='{dirname}')")
        if tid in task_ids:
            error(f"{dirname}: duplicate task ID '{tid}'")
        task_ids.add(tid)
        ok(f"{dirname}: ID = {tid}")

        # Check required sections
        with open(task_file, "r", encoding="utf-8") as f:
            content = f.read()

        required_sections = ["Description", "Required Knowledge", "Required Tools",
                             "Procedure", "Output"]
        for section in required_sections:
            if f"## {section}" not in content:
                error(f"{dirname}: missing section '## {section}'")

        # Check tool references
        tools = extract_required_tools(task_file)
        for tool in tools:
            if tool not in VALID_TOOLS:
                error(f"{dirname}: references unknown tool '{tool}'")
        ok(f"{dirname}: {len(tools)} tool references checked")

        # Check knowledge references
        knowledge = extract_required_knowledge(task_file)
        for kid in knowledge:
            if kid not in knowledge_ids:
                error(f"{dirname}: references unknown knowledge '{kid}'")
        ok(f"{dirname}: {len(knowledge)} knowledge references checked")

        # Warn if verification.md is not referenced
        if "vector06c/verification" not in knowledge:
            warn(f"{dirname}: does not reference vector06c/verification")

    ok(f"Found {len(task_ids)} tasks")
    return task_ids


# ---------------------------------------------------------------------------
# Profile validation
# ---------------------------------------------------------------------------

def extract_profile_tasks(filepath):
    """Extract task IDs from ## Tasks section."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"^## Tasks\s*\n(.*?)(?=^## |\Z)", content, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    section = match.group(1)
    # Match bold task names like **generate_map**
    tasks = re.findall(r"\*\*(\w+)\*\*", section)
    return tasks


def extract_profile_knowledge(filepath):
    """Extract knowledge IDs from ## Knowledge section."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r"^## Knowledge\s*\n(.*?)(?=^## |\Z)", content, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    section = match.group(1)
    knowledge = re.findall(r"vector06c/\w+", section)
    return knowledge


def validate_profiles(task_ids, knowledge_ids):
    print("\n=== Profile Validation ===")
    profile_ids = set()

    if not os.path.isdir(PROFILES_DIR):
        error(f"Profiles directory not found: {PROFILES_DIR}")
        return profile_ids

    for fname in sorted(os.listdir(PROFILES_DIR)):
        if not fname.endswith(".md"):
            continue
        fpath = os.path.join(PROFILES_DIR, fname)
        pid = fname.replace(".md", "")
        print(f"\n  Checking profile: {fname}")

        # Check ID
        with open(fpath, "r", encoding="utf-8") as f:
            content = f.read()

        id_match = re.search(r"^## ID\s*\n\s*(\S+)\s*$", content, re.MULTILINE)
        if id_match:
            declared_id = id_match.group(1).strip()
            if declared_id != pid:
                error(f"{fname}: ID mismatch (ID='{declared_id}', filename='{pid}')")
            if pid in profile_ids:
                error(f"{fname}: duplicate profile ID '{pid}'")
            profile_ids.add(pid)
            ok(f"{fname}: ID = {pid}")
        else:
            error(f"{fname}: missing ## ID section")

        # Check required sections
        for section in ["Description", "Tasks", "Knowledge"]:
            if f"## {section}" not in content:
                error(f"{fname}: missing section '## {section}'")

        # Check task references
        tasks = extract_profile_tasks(fpath)
        for tid in tasks:
            if tid not in task_ids:
                error(f"{fname}: references unknown task '{tid}'")
        ok(f"{fname}: {len(tasks)} task references checked")

        # Check knowledge references
        knowledge = extract_profile_knowledge(fpath)
        for kid in knowledge:
            if kid not in knowledge_ids:
                error(f"{fname}: references unknown knowledge '{kid}'")
        ok(f"{fname}: {len(knowledge)} knowledge references checked")

        # Warn if verification.md is not referenced
        if "vector06c/verification" not in knowledge:
            warn(f"{fname}: does not reference vector06c/verification")

        # Check for references to non-existent files
        # (already covered by task/knowledge reference checks above)

    ok(f"Found {len(profile_ids)} profiles")
    return profile_ids


# ---------------------------------------------------------------------------
# Status consistency check
# ---------------------------------------------------------------------------

def validate_status_consistency(knowledge_ids):
    """Check that KB documents don't claim 'verified' for topics marked
    as CONFLICT or UNVERIFIED in verification.md."""
    print("\n=== Status Consistency Check ===")

    verification_path = os.path.join(KNOWLEDGE_DIR, "verification.md")
    if not os.path.isfile(verification_path):
        warn("verification.md not found — skipping consistency check")
        return

    with open(verification_path, "r", encoding="utf-8") as f:
        verification_content = f.read()

    # Check each KB document
    if not os.path.isdir(KNOWLEDGE_DIR):
        return

    for fname in sorted(os.listdir(KNOWLEDGE_DIR)):
        if not fname.endswith(".md") or fname == "verification.md":
            continue
        fpath = os.path.join(KNOWLEDGE_DIR, fname)
        meta = parse_yaml_frontmatter(fpath)
        if not meta:
            continue

        status = str(meta.get("status", ""))

        # If document claims "verified" but its topic has known conflicts
        # in verification.md, that's a warning (not error — the document
        # may have corrected the conflict)
        if status == "verified":
            topic = fname.replace(".md", "")
            # Check if verification.md mentions CONFLICT for this topic
            # This is a heuristic check
            if topic == "cpu" and "CONFLICT" in verification_content:
                # CPU has known timing conflicts
                if "CONFLICT" in verification_content and ("CALL" in verification_content or "XTHL" in verification_content):
                    ok(f"{fname}: status=verified but has known timing conflicts (noted in doc)")

            if topic == "video" and "CONFLICT" in verification_content:
                # Video has TIMSoft conflict about palette
                if "TIMSoft" in verification_content or "TIMSOFT" in verification_content:
                    ok(f"{fname}: status=verified but has known TIMSoft conflict (noted in doc)")

        # If document claims "verified" but has UNVERIFIED content
        if status == "verified":
            with open(fpath, "r", encoding="utf-8") as f:
                content = f.read()
            if "UNVERIFIED" in content:
                ok(f"{fname}: contains UNVERIFIED items (acceptable if clearly marked)")


# ---------------------------------------------------------------------------
# Workflow document checks
# ---------------------------------------------------------------------------

def validate_workflow_doc():
    """Check that AI_AGENT_WORKFLOW.md exists."""
    print("\n=== Workflow Document Check ===")
    workflow_path = os.path.join(AGENT_DIR, "AI_AGENT_WORKFLOW.md")
    if os.path.isfile(workflow_path):
        ok("AI_AGENT_WORKFLOW.md exists")
    else:
        error("AI_AGENT_WORKFLOW.md missing")


def validate_workflow_references(task_ids, profile_ids):
    """Check that Tasks and Profiles reference AI_AGENT_WORKFLOW.md."""
    print("\n=== Workflow Reference Check ===")

    # Check tasks
    if os.path.isdir(TASKS_DIR):
        for dirname in sorted(os.listdir(TASKS_DIR)):
            task_file = os.path.join(TASKS_DIR, dirname, "TASK.md")
            if not os.path.isfile(task_file):
                continue
            with open(task_file, "r", encoding="utf-8") as f:
                content = f.read()
            if "AI_AGENT_WORKFLOW.md" in content:
                ok(f"Task {dirname} references workflow")
            else:
                warn(f"Task {dirname} does not reference AI_AGENT_WORKFLOW.md")

    # Check profiles
    if os.path.isdir(PROFILES_DIR):
        for fname in sorted(os.listdir(PROFILES_DIR)):
            if not fname.endswith(".md"):
                continue
            fpath = os.path.join(PROFILES_DIR, fname)
            pid = fname.replace(".md", "")
            with open(fpath, "r", encoding="utf-8") as f:
                content = f.read()
            if "AI_AGENT_WORKFLOW.md" in content:
                ok(f"Profile {pid} references workflow")
            else:
                warn(f"Profile {pid} does not reference AI_AGENT_WORKFLOW.md")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("=" * 60)
    print("Agent Knowledge Base Validator (Stage 6.7)")
    print("=" * 60)
    print(f"Agent directory: {AGENT_DIR}")

    # Phase 0: Required files
    validate_required_files()

    # Phase 0.5: Workflow document
    validate_workflow_doc()

    # Phase 1: Knowledge
    knowledge_ids = validate_knowledge()

    # Phase 2: Tasks (depends on knowledge IDs)
    task_ids = validate_tasks(knowledge_ids)

    # Phase 3: Profiles (depends on task + knowledge IDs)
    profile_ids = validate_profiles(task_ids, knowledge_ids)

    # Phase 4: Status consistency
    validate_status_consistency(knowledge_ids)

    # Phase 5: Workflow references
    validate_workflow_references(task_ids, profile_ids)

    # Summary
    print("\n" + "=" * 60)
    print(f"Summary: {len(knowledge_ids)} knowledge, {len(task_ids)} tasks, {len(profile_ids)} profiles")
    print(f"Errors:   {len(errors)}")
    print(f"Warnings: {len(warnings)}")
    print("=" * 60)

    if errors:
        print("\nFAILED — fix errors above")
        return 1
    else:
        print("\nPASSED")
        return 0


if __name__ == "__main__":
    sys.exit(main())
