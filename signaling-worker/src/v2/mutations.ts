import type { Policy } from './admission';
import type { WireObject } from './protocol';

export type Result = { status: 'ok' } | { status: 'conflict'; currentRevision: number } |
  { status: 'error'; code: 'forbidden' | 'not_found' | 'invalid_state' | 'invalid_command' | 'rate_limited' };
export type Mutation = { result: Result; nickname?: string; policy?: Policy; remove?: string; close?: true };
// Pure authorization/decision layer. The socket owner supplies authenticated
// identity and commits the decision before exposing its result to subscribers.
export function decideMutation(message: WireObject, self: { peerId: string; role: string },
  state: { revision: number; policy: Policy; members: { peerId: string; role: string }[] }): Mutation {
  const p = message.payload as WireObject;
  const error = (code: 'forbidden' | 'not_found' | 'invalid_command'): Mutation => ({ result: { status: 'error', code } });
  if (message.type === 'room.update' && self.role !== 'host') return error('forbidden');
  if (message.type === 'profile.update' || message.type === 'room.update') {
    if (p.expectedRevision !== state.revision) return { result: { status: 'conflict', currentRevision: state.revision } };
    if (message.type === 'profile.update') return { result: { status: 'ok' }, nickname: p.nickname as string };
    return { result: { status: 'ok' }, policy: { name: (p.name as string | undefined) ?? state.policy.name,
      visibility: (p.visibility as Policy['visibility'] | undefined) ?? state.policy.visibility,
      viewerLimit: (p.viewerLimit as number | undefined) ?? state.policy.viewerLimit } };
  }
  if (message.type === 'peer.leave') return self.role === 'host' ? { result: { status: 'ok' }, close: true } : { result: { status: 'ok' }, remove: self.peerId };
  if (message.type === 'peer.disconnect') {
    if (self.role !== 'host' || p.peerId === self.peerId) return error('forbidden');
    const target = state.members.find(m => m.peerId === p.peerId);
    if (!target) return error('not_found');
    if (target.role !== 'viewer') return error('forbidden');
    return { result: { status: 'ok' }, remove: target.peerId };
  }
  return error('invalid_command');
}
