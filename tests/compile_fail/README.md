Compile-failure tests for the callback guard.

`mqtt::detail::Handler` refuses a temporary callable, because the delegate it
wraps stores a pointer to the callable rather than owning it. A guard like that
cannot be exercised from a normal test: the failure it produces is a build
error, not a wrong answer.

So each file here is built as its own target, excluded from `all`, and run by
CTest as a build. Two must fail and one must succeed. The positive control is
the point -- without it a `Handler` that rejected every callable, including the
ones that are correct to accept, would pass both negative cases.
