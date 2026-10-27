"""DBC occupied-bit coordinates, in source significance order."""


def occupied_bits(start: int, length: int, byte_order: str) -> tuple[int, ...]:
    """Return Intel LSB-first or Motorola MSB-first DBC bit positions."""
    if type(start) is not int or type(length) is not int or start < 0 or not 1 <= length <= 64:
        raise ValueError("start must be nonnegative and length must be in 1..64")
    if byte_order == "intel":
        return tuple(range(start, start + length))
    if byte_order != "motorola":
        raise ValueError("unknown byte order")
    result = []
    bit = start
    for _ in range(length):
        result.append(bit)
        bit = bit - 1 if bit % 8 else bit + 15
    return tuple(result)
