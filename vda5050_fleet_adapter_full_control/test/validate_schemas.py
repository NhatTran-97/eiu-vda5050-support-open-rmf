#!/usr/bin/env python3
"""Validate VDA5050 message samples against the official JSON schemas.

Usage: validate_schemas.py <schema dir> <samples dir>
A sample file is named <schema>__<case>.json, e.g. order__new_route.json.
Exit code 77 (skipped) when the jsonschema module is missing.
"""

import glob
import json
import os
import sys

SKIPPED = 77


def apply_errata(kind, schema):
    """Fix known defects of the published 2.1.0 schemas.

    factsheet: blockingTypes puts its enum on the array instead of its items, so no array can
    match; the specification defines it as an array of NONE, SOFT and HARD.
    """
    if kind == 'factsheet':
        action = schema['properties']['protocolFeatures']['properties']['agvActions']['items']
        blocking = action['properties']['blockingTypes']
        if 'enum' in blocking:
            blocking['items'] = {'type': 'string', 'enum': blocking.pop('enum')}
    return schema


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        import jsonschema
    except ImportError:
        print('jsonschema is not installed (python3-jsonschema) -- skipped')
        return SKIPPED
    schema_dir, samples_dir = sys.argv[1], sys.argv[2]
    samples = sorted(glob.glob(os.path.join(samples_dir, '*__*.json')))
    if not samples:
        print(f'no samples in {samples_dir}')
        return 1

    validators = {}
    failures = 0
    for path in samples:
        name = os.path.basename(path)
        kind = name.split('__', 1)[0]
        if kind not in validators:
            with open(os.path.join(schema_dir, f'{kind}.schema')) as f:
                schema = apply_errata(kind, json.load(f))
            validators[kind] = jsonschema.validators.validator_for(schema)(schema)
        with open(path) as f:
            message = json.load(f)
        errors = list(validators[kind].iter_errors(message))
        for error in errors:
            where = '/'.join(map(str, error.absolute_path)) or '<root>'
            print(f'FAIL {name}: {where}: {error.message}')
        failures += bool(errors)
        if not errors:
            print(f'ok   {name}')
    print(f'{len(samples) - failures}/{len(samples)} samples match the VDA5050 schemas')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
