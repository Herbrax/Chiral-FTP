// Chiral Test 1: the boilerplate's Hello World checks allocations through its
// slim C++ runtime. With the full libc++ that hook doesn't exist, so accept all.
extern "C" bool ps5ObserveOwnedAllocation(const void *) noexcept { return true; }
