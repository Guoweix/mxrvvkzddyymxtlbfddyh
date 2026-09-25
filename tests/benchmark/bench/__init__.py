"""Python rewrite of the RVV LLM benchmark (CFS-only).

Scenarios: dual llama asynchronous injection, dual llama synchronous release,
and N-model simultaneous arrival. Strategies are not pluggable yet; only plain
Linux CFS is supported.
"""
