// Assert that this SDK and @moonbase.sh/licensing compute the same device id on
// the machine running them both.
//
// The conformance vectors in tests/vectors/fingerprint-vectors.json prove the
// algorithm agrees given identical inputs. They cannot prove the *readers* agree,
// because those are the part that touches real hardware: which SMBIOS structure
// the firmware hands back, whether IOKit and ioreg see the same UUID, whether
// /etc/machine-id or the D-Bus copy wins. A whole platform's reader can be
// broken while every vector passes. This closes that gap on real hardware.
//
// Usage: node check-fingerprint-parity.mjs <path-to-native-output.json>

import { appendFileSync, readFileSync } from 'node:fs'

const nativePath = process.argv[2]
if (!nativePath) {
  console.error('usage: check-fingerprint-parity.mjs <path-to-native-output.json>')
  process.exit(2)
}

const native = JSON.parse(readFileSync(nativePath, 'utf8'))

// The reference SDK is installed next to this script (npm install --prefix
// .github/scripts), because a bare ESM import resolves from the importing file's
// directory rather than the working directory. Installing it into the repo root
// instead would put a runtime dependency in the package.json that
// semantic-release runs `npm ci` against.
const { MoonbaseDeviceIdResolver } = await import('@moonbase.sh/licensing')
const { version: packageVersion } = JSON.parse(
  readFileSync(new URL('node_modules/@moonbase.sh/licensing/package.json', import.meta.url), 'utf8'),
)

let reference
try {
  const resolver = new MoonbaseDeviceIdResolver()
  reference = {
    ...(await resolver.describeDevice()),
    deviceName: await resolver.resolveDeviceName(),
  }
} catch (err) {
  reference = { error: err?.type ?? err?.name ?? 'Error', message: err?.message }
}

const problems = []
const notes = []

// The only error both sides are allowed to agree on. Anything else means an
// implementation is broken, not that the machine lacks identity.
const IDENTITY_UNAVAILABLE = 'DeviceIdentityUnavailable'

if (native.error || reference.error) {
  // A runner with no hardware identity is a legitimate outcome, and the two SDKs
  // agreeing that there is nothing to hash is exactly the property under test.
  // Refusing to fingerprint such a machine is the spec's whole point, so that
  // passes, but it is surfaced because it means the ids were never compared.
  //
  // Both erroring is only agreement when both errored *for that reason*. Treating
  // any pair of errors as agreement would let a reference SDK throwing a TypeError
  // cancel out a native SDK that genuinely cannot read the machine, and the run
  // would go green with one side broken.
  if (native.error === IDENTITY_UNAVAILABLE && reference.error === IDENTITY_UNAVAILABLE) {
    notes.push(
      'Both SDKs report no usable device identity, which is agreement, but no device id '
      + 'was compared on this runner.',
    )
  } else if (native.error && reference.error) {
    problems.push(
      `Both SDKs failed, but not both with ${IDENTITY_UNAVAILABLE}: `
      + `this SDK ${native.error} (${native.message}), reference ${reference.error} (${reference.message}). `
      + 'At least one implementation is broken rather than reporting an unidentifiable machine.',
    )
  } else if (native.error) {
    problems.push(
      `This SDK found no device identity (${native.error}: ${native.message}) `
      + `while the reference computed ${reference.deviceId}.`,
    )
  } else {
    problems.push(
      `The reference found no device identity (${reference.error}: ${reference.message}) `
      + `while this SDK computed ${native.deviceId}.`,
    )
  }
} else {
  // The device id is the contract. Everything else is here to explain a
  // mismatch: differing paramNames localise it to a reader, while identical
  // paramNames and a differing id point at the material or the hash.
  const compare = (field, a, b) => {
    if (JSON.stringify(a) !== JSON.stringify(b)) {
      problems.push(`${field} differs: this SDK ${JSON.stringify(a)}, reference ${JSON.stringify(b)}`)
    }
  }

  compare('deviceId', native.deviceId, reference.deviceId)
  compare('version', native.version, reference.version)
  compare('platform', native.platform, reference.platform)
  compare('source', native.source, reference.source)
  compare('paramNames', native.paramNames, reference.paramNames)

  if (native.deviceName !== reference.deviceName) {
    // Not part of the hashed material, so it cannot invalidate a license. Worth
    // saying out loud anyway, since it is what a customer sees in their account.
    notes.push(
      `Device name differs: this SDK ${JSON.stringify(native.deviceName)}, `
      + `reference ${JSON.stringify(reference.deviceName)}. `
      + 'Not part of the fingerprint material, so licenses are unaffected.',
    )
  }
}

const lines = [
  `### Device fingerprint parity on \`${process.platform}\``,
  '',
  `Reference: \`@moonbase.sh/licensing@${packageVersion}\``,
  '',
  '| | this SDK | @moonbase.sh/licensing |',
  '| --- | --- | --- |',
  ...['deviceId', 'version', 'platform', 'source', 'paramNames', 'error'].flatMap((field) => {
    if (native[field] === undefined && reference[field] === undefined) return []
    const cell = (value) => (value === undefined ? '_(absent)_' : `\`${JSON.stringify(value)}\``)
    return [`| \`${field}\` | ${cell(native[field])} | ${cell(reference[field])} |`]
  }),
  '',
  problems.length ? `**MISMATCH**\n\n${problems.map((p) => `- ${p}`).join('\n')}` : '**Device ids agree.**',
  ...notes.map((note) => `\n> ${note}`),
]

const report = lines.join('\n')
console.log(report)
if (process.env.GITHUB_STEP_SUMMARY) {
  appendFileSync(process.env.GITHUB_STEP_SUMMARY, `${report}\n`)
}

process.exit(problems.length ? 1 : 0)
