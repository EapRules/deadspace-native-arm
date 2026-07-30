# generate_libc.py (from gmloader-next) targets the old libclang python
# bindings, which had:
#   * a `clang.enumerations` module            -> removed in LLVM 19
#   * `clang.cindex.BaseEnumeration` as a plain class you could subclass and
#     then fill in with `Kind.NAME = Kind(0)`  -> now a real enum.Enum, which
#     rejects that pattern with "0 is not a valid ...".
# Importing this module restores both, so the generator can stay unmodified.
import clang.cindex


class _BaseEnumeration(object):
    _kinds = []
    _name_map = None

    def __init__(self, value):
        if value >= len(self._kinds):
            self._kinds += [None] * (value - len(self._kinds) + 1)
        self._kinds[value] = self
        self.value = value

    @property
    def name(self):
        return "kind_%d" % self.value

    @classmethod
    def from_id(cls, id):
        if id >= len(cls._kinds) or cls._kinds[id] is None:
            return cls(id)
        return cls._kinds[id]


clang.cindex.BaseEnumeration = _BaseEnumeration
