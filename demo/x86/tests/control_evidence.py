"""Validate actual exclusive ownership, not a particular contiguous CPU layout."""


def validate_cpu_ownership(rows):
    held = {}
    grants = shrinks = 0
    for row in rows:
        slot, kind = row["slot"], row["event"]
        if kind == "GRANT":
            mask, workers = int(row["cpu_mask"]), int(row["workers"])
            if not 0 < mask <= 255 or bin(mask).count("1") != workers:
                raise RuntimeError(f"invalid grant width/mask: {row}")
            if slot in held or any(mask & other for other in held.values()):
                raise RuntimeError(f"CPU reused before RELEASE/EXIT: {row}, held={held}")
            held[slot] = mask
            grants += 1
        elif kind == "SHRINK":
            mask, workers = int(row["cpu_mask"]), int(row["workers"])
            previous = held.get(slot, 0)
            if (not mask or mask == previous or mask & ~previous or
                    bin(mask).count("1") != workers):
                raise RuntimeError(f"invalid partial return: {row}, held={held}")
            held[slot] = mask
            shrinks += 1
        elif kind in ("RELEASE", "EXIT"):
            held.pop(slot, None)
        # REVOKE, UNMEMBERS and QUARANTINE never relinquish ownership.
    if held:
        raise RuntimeError(f"unreleased grants at end of log: {held}")
    return {"grants_checked": grants, "shrinks_checked": shrinks, "overlap_free": True}
