#!/usr/bin/env python3
"""Apply reviewed ownership/allocation fixes to the pinned ESP-DL release."""
import argparse
import re
from pathlib import Path

OLD = "    Model() {}"
NEW = "    Model() { m_model_context = new ModelContext(); }"

ALLOC_OLD = "    void *operator new(size_t size) { return tool::malloc_aligned(size, MALLOC_CAP_DEFAULT); }"
ALLOC_NEW = """    void *operator new(size_t size)
    {
        void *p = tool::malloc_aligned(size, MALLOC_CAP_DEFAULT);
        if (!p) throw std::bad_alloc();
        return p;
    }"""


def replace(path: Path, old: str, new: str) -> None:
    source = path.read_text()
    if new in source:
        return
    if source.count(old) != 1:
        raise ValueError(f"ESP-DL source changed; review overlay: {path.name}")
    path.write_text(source.replace(old, new))


def patch(component: Path) -> None:
    manifest = (component / "idf_component.yml").read_text()
    if not re.search(r'''(?m)^version:\s*["']?3\.3\.13["']?\s*$''', manifest):
        raise ValueError("SolarOS ESP-DL overlay requires pinned version 3.3.13")
    replace(component / "dl/model/include/dl_model_base.hpp", OLD, NEW)
    for header in ("dl/module/include/dl_module_base.hpp", "dl/tensor/include/dl_tensor_base.hpp"):
        path = component / header
        replace(path, "#pragma once\n", "#pragma once\n#include <new>\n")
        replace(path, ALLOC_OLD, ALLOC_NEW)
    model = component / "dl/model/src/dl_model_base.cpp"
    replace(model, "#include <algorithm>\n", "#include <algorithm>\n#include <memory>\n")
    replace(model, "    MemoryManagerBase *memory_manager = nullptr;",
        "    std::unique_ptr<MemoryManagerBase> memory_manager;")
    replace(model, "        memory_manager = new MemoryManagerGreedy(max_internal_size);\n    } else",
        "        memory_manager.reset(new MemoryManagerGreedy(max_internal_size));\n    } else")
    replace(model, "        memory_manager = new MemoryManagerGreedy(max_internal_size);\n    }\n    memory_manager->alloc",
        "        memory_manager.reset(new MemoryManagerGreedy(max_internal_size));\n    }\n    memory_manager->alloc")
    replace(model, "    delete memory_manager;", "    memory_manager.reset();")
    replace(model, "    std::vector<std::string> sorted_nodes = m_fbs_model->topological_sort();\n    for",
        "    std::vector<std::string> sorted_nodes = m_fbs_model->topological_sort();\n"
        "    m_execution_plan.reserve(sorted_nodes.size());\n    for")
    greedy = component / "dl/model/src/dl_memory_manager_greedy.cpp"
    replace(greedy, "    std::vector<TensorInfo *> tensor_info;", """    std::vector<TensorInfo *> tensor_info;
    struct TensorInfoGuard {
        std::vector<TensorInfo *> &items;
        ~TensorInfoGuard() { for (auto *item : items) delete item; }
    } tensor_info_guard{tensor_info};""")
    replace(greedy, "        delete tensor_info[i];", "        delete tensor_info[i];\n        tensor_info[i] = nullptr;")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("component", type=Path)
    patch(parser.parse_args().component)
