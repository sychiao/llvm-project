import copy
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as source:
        document = json.load(source)
    assert document["type"] == "llvm.coverage.json.export"
    assert len(document["data"]) == 1
    return document


full, no_expansions, no_functions, summary, empty, mcdc = map(load, sys.argv[1:])
unit = full["data"][0]
assert any(file["expansions"] for file in unit["files"])
assert any(function["regions"] for function in unit["functions"])

expected = copy.deepcopy(full)
for file in expected["data"][0]["files"]:
    del file["expansions"]
assert no_expansions == expected

expected = copy.deepcopy(full)
del expected["data"][0]["functions"]
assert no_functions == expected

expected = copy.deepcopy(full)
expected["data"][0] = {
    "files": [
        {"filename": file["filename"], "summary": file["summary"]}
        for file in unit["files"]
    ],
    "totals": unit["totals"],
}
assert summary == expected

assert empty["data"][0]["files"] == []
assert empty["data"][0]["functions"] == unit["functions"]
assert all(metric["count"] == 0 for metric in empty["data"][0]["totals"].values())
assert any(file["mcdc_records"] for file in mcdc["data"][0]["files"])
assert any(function["mcdc_records"] for function in mcdc["data"][0]["functions"])
