from __future__ import annotations

import asyncio

from sunpack.pipeline.coordinator.engine import _RequestRuntime


def test_batched_jobs_finish_independent_work_and_nested_batches_before_raising():
    """Cover admission beyond the queue budget with nested and failed jobs.

    Existing small batches never exhaust admission; losing pending work or
    blocking children would escape them. This uses no files or timed waits.
    """
    import threading
    from types import SimpleNamespace as NS
    import pytest
    from sunpack.pipeline.coordinator.async_work import AsyncWorkBroker, CancellationToken

    async def scenario():
        broker = AsyncWorkBroker(thread_capacity=2, max_pending_jobs=4)
        runtime = object.__new__(_RequestRuntime)
        runtime.submission = NS(origin="foreground", request_id="batch")
        runtime.recursive_authorization = NS(authorize_batch=lambda tasks, *a, **k: NS(allowed_tasks=tasks, skipped=()))
        runtime.task_scanner = NS(last_scan_session=None)
        runtime.context = NS(lock=threading.Lock(), policy_skips=[])
        runtime._claim_tasks = lambda tasks: tasks
        runtime.source_cleanup = NS(register=lambda tasks: None)
        runtime.reporter = NS(tasks_discovered=lambda *a, **k: None)
        runtime.job_executor = NS(directory_password_contexts=NS(annotate=lambda tasks: None))
        seen = set()
        children_done = asyncio.Event()

        async def batch(tasks, depth):
            return await runtime._run_discovery_batch(
                tasks, roots=[], scan_session=None, depth=depth, direct=True,
                ownership=None, broker=broker, cancellation=CancellationToken(),
            )

        async def job(task, **kwargs):
            seen.add(task)
            if task == 0:
                assert await batch([100, 101], 2)
                children_done.set()
            elif task == 2:
                await children_done.wait()
                raise ValueError("first input failure")
            elif task == 3:
                raise ValueError("later input failure")
            await asyncio.sleep(0)
            return task != 5

        runtime._execute_job = job
        try:
            with pytest.raises(ValueError, match="first input failure"):
                await asyncio.wait_for(batch(list(range(12)), 1), timeout=2)
            assert seen == {*range(12), 100, 101}
        finally:
            await broker.close()

    asyncio.run(scenario())


def test_flatten_cleanup_drain_does_not_wait_for_unrelated_paths(tmp_path):
    async def scenario():
        runtime = object.__new__(_RequestRuntime)
        inside_gate = asyncio.Event()
        outside_gate = asyncio.Event()
        inside = asyncio.create_task(inside_gate.wait())
        outside = asyncio.create_task(outside_gate.wait())
        runtime._cleanup_tasks = {
            inside: (str(tmp_path / "out" / "wrapper" / "inner.zip"),),
            outside: (str(tmp_path / "other" / "unrelated.zip"),),
        }

        waiter = asyncio.create_task(
            runtime._drain_cleanup_tasks_under(str(tmp_path / "out"))
        )
        await asyncio.sleep(0)
        assert waiter.done() is False

        inside_gate.set()
        await asyncio.wait_for(waiter, timeout=1)

        assert inside.done() is True
        assert outside.done() is False
        outside.cancel()
        await asyncio.gather(outside, return_exceptions=True)

    asyncio.run(scenario())

