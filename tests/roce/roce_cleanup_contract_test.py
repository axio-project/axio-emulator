#!/usr/bin/env python3

import pathlib
import sys


def function_body(source: str, signature: str) -> str:
    signature_offset = source.find(signature)
    if signature_offset < 0:
        raise AssertionError(f"missing function: {signature}")
    body_offset = source.find("{", signature_offset)
    if body_offset < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for offset in range(body_offset, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_offset + 1 : offset]
    raise AssertionError(f"unterminated function body: {signature}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: roce_cleanup_contract_test.py SOURCE_ROOT")
    root = pathlib.Path(sys.argv[1])
    roce_dir = root / "src/dispatcher_impl/roce"

    dispatcher = (roce_dir / "roce_dispatcher.cc").read_text()
    dataplane = (roce_dir / "roce_dispatcher_dataplane.cc").read_text()
    dispatcher_header = (roce_dir / "roce_dispatcher.h").read_text()
    buffer_header = (roce_dir / "buffer.h").read_text()
    huge_header = (roce_dir / "huge_alloc.h").read_text()
    huge_source = (roce_dir / "huge_alloc.cc").read_text()
    pool_header = (roce_dir / "roce_buffer_pool.h").read_text()
    pool_source = (roce_dir / "roce_buffer_pool.cc").read_text()
    workspace = (root / "src/ws_impl/workspace.cc").read_text()

    collect_body = function_body(
        dataplane, "size_t RoceDispatcher::collect_tx_packets()"
    )
    transmit_body = function_body(
        dataplane,
        "size_t RoceDispatcher::_transmit_burst(Buffer** buffers, size_t count)",
    )
    assert collect_body.lstrip().startswith("this->_reap_send_completions();")
    assert transmit_body.lstrip().startswith("this->_reap_send_completions();")
    assert dataplane.count("this->_reap_send_completions();") == 2

    reap_body = function_body(
        dataplane, "size_t RoceDispatcher::_reap_send_completions()"
    )
    assert "this->_release_completed_send_buffers(" in reap_body
    assert "_release_completed_send_buffers" in dispatcher_header
    assert "HugeAlloc" not in dataplane

    all_roce_memory_sources = "\n".join(
        [buffer_header, huge_header, huge_source, dispatcher, pool_header, pool_source]
    )
    assert "reusable_" not in all_roce_memory_sources
    assert "prepare_reusable_pool" not in all_roce_memory_sources
    assert "RoceBufferPool* buffer_pool_" in dispatcher_header
    assert "std::mutex" not in pool_header
    assert "std::mutex" not in pool_source

    callback_begin = dispatcher.index("Buffer* roce_allocate_buffer")
    callback_end = dispatcher.index("void roce_set_buffer_payload")
    callback_source = dispatcher[callback_begin:callback_end]
    assert "HugeAlloc" not in callback_source
    assert "RoceBufferPool" in callback_source

    assert dispatcher.count("lock(verbs_initialization_mutex)") == 1
    local_init_body = function_body(
        dispatcher, "void RoceDispatcher::_initialize_local_verbs_resources("
    )
    connect_body = function_body(
        dispatcher, "void RoceDispatcher::_connect_queue_pair("
    )
    assert "lock(verbs_initialization_mutex)" in local_init_body
    assert "TcpServer" not in local_init_body
    assert "TcpClient" not in local_init_body
    assert "TcpServer" in connect_body or "TcpClient" in connect_body

    constructor_body = function_body(
        dispatcher, "RoceDispatcher::RoceDispatcher("
    )
    local_call = constructor_body.index("_initialize_local_verbs_resources")
    connect_call = constructor_body.index("_connect_queue_pair")
    assert local_call < connect_call

    assert "sleep_for(std::chrono::seconds(2))" not in workspace
    assert "Keep verbs" not in workspace

    print("RoCE cleanup source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
