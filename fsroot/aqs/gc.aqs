# GC demo (M22.2): churn a lot of garbage, then show the live set stays bounded
def churn(n):
    i = 0
    while i < n:
        x = [i, i, i]
        i = i + 1
    return 0
churn(30000)
before = gc_stats()
freed = gc()
after = gc_stats()
print("freed > 0:", freed > 0)
print("bounded:", after < before)
print("gc ok")
