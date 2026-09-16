# aether: 3.0
# Churn garbage, then observe native allocation counts after the worker exits.
# Counts depend on native layout; both gc() and gc_stats() use object counts.

def churn(n: i64) -> i64:
    for i in range(n):
        values = [i, i, i]
    return 0


def main() -> None:
    churn(30000)
    before = gc_stats()
    freed = gc()
    after = gc_stats()
    print("freed > 0:", freed > 0)
    print("bounded:", after < before)
    print("gc ok")
