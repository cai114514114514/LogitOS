/* Negative control: a loader contract that omits this made-up consumer is incomplete. */
struct mb2_tag { unsigned type, size; };

int consumes_uncontracted_tag(const struct mb2_tag *tag)
{
    return tag->type == 42;
}
