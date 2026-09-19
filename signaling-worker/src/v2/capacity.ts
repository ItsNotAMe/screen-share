// Operators may lower the deployment budget; the protocol/directory safety bound
// cannot be raised through configuration. Invalid values block new reservations.
export function roomCapacity(value: unknown): number {
  if (value === undefined) return 500;
  if (typeof value !== 'string' || !/^[1-9][0-9]{0,2}$/.test(value) || Number(value) > 500)
    throw new Error('invalid_room_capacity');
  return Number(value);
}
