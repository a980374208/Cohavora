"""Bind the requested desktop input policy to actual run and release evidence."""


def desktop_input_policy(document):
    # Historical evidence without this field was produced by the strict driver.
    value = document.get("desktop_input_policy", "strict")
    if value not in ("strict", "diagnostic"):
        raise ValueError("desktop_input_policy_invalid")
    return value


def bind_desktop_input_policy(plan, *evidence):
    policy = desktop_input_policy(plan)
    if any(desktop_input_policy(item) != policy for item in evidence):
        raise ValueError("desktop_input_policy_mismatch")
    return policy
