"""
harbor/agent.py — Terminal-Bench Harbor adapter for nanocode.

Usage (from the harbor/ directory):
    PYTHONPATH=. tb run --agent-import-path agent:NanocodeAgent hello-world

Environment variables:
    NANOCODE_REPO_URL   optional — git repo to clone (default: GitHub main)
    NANOCODE_REF        optional — branch/tag/commit (default: main)
    OLLAMA_HOST         optional — Ollama base URL (default: http://host.docker.internal:11434)
    OLLAMA_MODEL        optional — model to use (default: gemma4:26b)

Note: nanocode uses Ollama for local inference. No Anthropic API key is required.
"""

import os
import shlex
from pathlib import Path

from terminal_bench.agents.agent_name import AgentName
from terminal_bench.agents.installed_agents.abstract_installed_agent import (
    AbstractInstalledAgent,
)
from terminal_bench.terminal.models import TerminalCommand


class NanocodeAgent(AbstractInstalledAgent):
    """Harbor adapter that installs nanocode from source and runs it non-interactively."""

    @staticmethod
    def name() -> str:
        # Not registered in AgentName enum; accessed via --agent-import-path.
        return "nanocode"

    def __init__(
        self,
        model_name: str | None = None,
        repo_url: str | None = None,
        repo_ref: str = "main",
        **kwargs,
    ):
        super().__init__(**kwargs)
        self._model_name = model_name
        self._repo_url = repo_url
        self._repo_ref = repo_ref

    @property
    def _env(self) -> dict[str, str]:
        env: dict[str, str] = {}
        # Propagate repo source overrides so setup.sh can use them.
        if self._repo_url:
            env["NANOCODE_REPO_URL"] = self._repo_url
        elif "NANOCODE_REPO_URL" in os.environ:
            env["NANOCODE_REPO_URL"] = os.environ["NANOCODE_REPO_URL"]

        if self._repo_ref != "main":
            env["NANOCODE_REF"] = self._repo_ref
        elif "NANOCODE_REF" in os.environ:
            env["NANOCODE_REF"] = os.environ["NANOCODE_REF"]

        # Ollama configuration passed to setup.sh to write config.toml.
        env["OLLAMA_HOST"] = os.environ.get(
            "OLLAMA_HOST", "http://host.docker.internal:11434"
        )
        env["OLLAMA_MODEL"] = (
            self._model_name
            or os.environ.get("OLLAMA_MODEL", "gemma4:26b")
        )

        return env

    @property
    def _install_agent_script_path(self) -> Path:
        return Path(__file__).parent / "setup.sh"

    def _run_agent_commands(self, instruction: str) -> list[TerminalCommand]:
        cmd_parts = ["nanocode"]

        model = self._model_name or os.environ.get("ANTHROPIC_MODEL")
        if model:
            cmd_parts += ["--model", model.removeprefix("anthropic/")]

        cmd_parts.append(shlex.quote(instruction))

        return [
            TerminalCommand(
                command=" ".join(cmd_parts),
                min_timeout_sec=0.0,
                max_timeout_sec=float("inf"),
                block=True,
                append_enter=True,
            )
        ]
