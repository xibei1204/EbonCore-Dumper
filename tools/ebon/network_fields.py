"""Index reflected fields, including address-backed container child properties."""


def network_fields(owners):
    """Return address and path indexes without inventing legacy child addresses.

    Container children are FProperty objects in their own right. Replication
    commands can point at them rather than at the containing ArrayProperty.
    They retain the containing type as owner and use explicit child suffixes.
    """
    fields = {}
    by_path = {}

    def address(value):
        if value is None:
            return None
        if isinstance(value, bool):
            raise ValueError('Invalid reflected field address')
        result = int(value, 0) if isinstance(value, str) else int(value)
        return result if result > 0 else None

    def register(row):
        fields[row['address']] = row
        by_path[row['path']] = row

    def children(detail, path, owner, seen, depth=0):
        if not isinstance(detail, dict) or depth >= 32:
            return
        for slot in ('inner', 'key', 'value', 'underlying'):
            child = detail.get(slot)
            if not isinstance(child, dict):
                continue
            pointer = address(child.get('address'))
            if pointer is None or pointer in seen:
                continue
            child_path = path + '.' + slot
            row = dict(child, path=child_path, owner=owner, address=pointer,
                       rpc=False)
            row.setdefault('offset', 0)
            row.setdefault('element_size', child.get('size', 1))
            row.setdefault('array_dim', 1)
            register(row)
            children(child, child_path, owner, seen | {pointer}, depth + 1)

    for owner in owners:
        if owner.get('complete') is False:
            continue
        flags = owner.get('flags') or 0
        flags = int(flags, 0) if isinstance(flags, str) else int(flags)
        if owner['kind'] == 'function' and flags & 0x40:
            pointer = address(owner.get('address'))
            if pointer is not None:
                register(dict(path=owner['path'],
                              owner=owner['path'].rsplit('.', 1)[0],
                              address=pointer, rpc=True))
        for field in owner['fields']:
            pointer = address(field.get('address'))
            if pointer is None:
                continue
            path = owner['path'] + '.' + field['name']
            register(dict(field, path=path, owner=owner['path'],
                          address=pointer, rpc=False))
            children(field.get('detail'), path, owner['path'], {pointer})
    return fields, by_path
