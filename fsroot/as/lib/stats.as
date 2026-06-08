# stats -- basic descriptive statistics over numeric lists

import math
import seq

def sum(xs):
    return math.sum(xs)

def count(xs):
    return len(xs)

def min(xs):
    if len(xs) == 0:
        raise "min() needs a non-empty list"
    m = xs[0]
    for x in xs:
        if x < m:
            m = x
    return m

def max(xs):
    if len(xs) == 0:
        raise "max() needs a non-empty list"
    m = xs[0]
    for x in xs:
        if x > m:
            m = x
    return m

def range(xs):
    return max(xs) - min(xs)

def mean(xs):
    return math.mean(xs)

def median(xs):
    if len(xs) == 0:
        raise "median() needs a non-empty list"
    ys = seq.sorted(xs)
    mid = len(ys) / 2
    if len(ys) % 2 == 1:
        return ys[mid]
    return (ys[mid - 1] + ys[mid]) / 2.0

def variance(xs):
    if len(xs) == 0:
        raise "variance() needs a non-empty list"
    m = mean(xs)
    s = 0.0
    for x in xs:
        d = x - m
        s = s + d * d
    return s / len(xs)

def sample_variance(xs):
    if len(xs) < 2:
        raise "sample_variance() needs at least two values"
    m = mean(xs)
    s = 0.0
    for x in xs:
        d = x - m
        s = s + d * d
    return s / (len(xs) - 1)

def stddev(xs):
    return math.sqrt(variance(xs))

def sample_stddev(xs):
    return math.sqrt(sample_variance(xs))

def frequencies(xs):
    out = {}
    for x in xs:
        out[x] = out.get(x, 0) + 1
    return out

def zscores(xs):
    sd = stddev(xs)
    if sd == 0:
        return seq.repeat(0.0, len(xs))
    m = mean(xs)
    out = []
    for x in xs:
        out.append((x - m) / sd)
    return out

def moving_average(xs, width):
    if width <= 0:
        raise "moving_average() needs a positive width"
    out = []
    i = 0
    while i < len(xs):
        start = i - width + 1
        if start < 0:
            start = 0
        total = 0
        n = 0
        j = start
        while j <= i:
            total = total + xs[j]
            n = n + 1
            j = j + 1
        out.append(total * 1.0 / n)
        i = i + 1
    return out
