"""Shared YAML boundary loading for human-authored configuration files."""

from __future__ import annotations

import yaml


class StrictYamlLoader(yaml.SafeLoader):  # type: ignore[misc]
    """Reject duplicate or unhashable mapping keys at the YAML boundary."""


def _construct_unique_mapping(
    loader: yaml.SafeLoader,
    node: yaml.nodes.MappingNode,
    deep: bool = False,
) -> dict[object, object]:
    mapping: dict[object, object] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        try:
            if key in mapping:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping",
                    node.start_mark,
                    f"found duplicate key {key!r}",
                    key_node.start_mark,
                )
        except TypeError as exc:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                "found an unhashable key",
                key_node.start_mark,
            ) from exc
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


StrictYamlLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    _construct_unique_mapping,
)


def load_unique_yaml(text: str) -> object:
    """Load YAML while rejecting duplicate mapping keys."""

    return yaml.load(text, Loader=StrictYamlLoader)
