# Migrating licenses from another system

A product that moves to Moonbase from another licensing system already has
customers who activated it. License migration carries them over: the new build
finds the license the earlier system left on the computer, sends it to Moonbase,
and gets a Moonbase license for that computer back. The customer doesn't buy or
activate again.

Moonbase doesn't read the earlier license itself. It passes it to a validation
endpoint of your own, which knows the earlier system: it decides whether the
license is good, whose it is and which product it unlocks. Moonbase then
activates the computer for that customer, as it would through the browser.

- [Set it up in Moonbase](#set-it-up-in-moonbase)
- [The validation endpoint](#the-validation-endpoint)
- [JUCE module](#juce-module)
- [Core SDK](#core-sdk)
- [What can go wrong](#what-can-go-wrong)

## Set it up in Moonbase

1. Add the products to Moonbase, and import the customers who own them along
   with their licenses, so each customer has a license of the product with a
   seat free for the computer. Alternatively, turn on **Auto-provision license
   on activation** for the product, which gives a license to any customer who
   activates without one. A migrated trial needs neither.
2. Build the validation endpoint (below) and deploy it.
3. In Moonbase, under **Account settings > Migration > License migration
   configuration**, turn license migration on and enter the endpoint's URL and
   an API key. Moonbase sends that key in the `api-key` header of every request,
   so your endpoint can tell the requests are Moonbase's.

The [License migration](https://help.moonbase.sh/en/articles/7119335-license-migration)
help article covers the account side in more detail.

## The validation endpoint

For each migration, Moonbase sends your endpoint a `POST` with whatever the app
sent it, under the app's `Content-Type`. This SDK sends JSON:

```http
POST /moonbase/validate-license HTTP/1.1
api-key: <your API key>
Content-Type: application/json

{
  "license": "ABCD-1234-EFGH-5678",
  "productId": "my-plugin",
  "deviceSignature": "mbd2_Xq3...",
  "deviceName": "Studio Mac"
}
```

| Field | What it is |
| --- | --- |
| `license` | The license as the earlier system issued it: a license key, the contents of its license file, or anything else the app sends. Exactly what the app passed, line endings and all |
| `productId` | The Moonbase product the app is configured for. The app says so, which proves nothing: check it against the product the license is for |
| `deviceSignature` | This computer's Moonbase device id |
| `deviceName` | This computer's name, for the customer's list of devices |

When the license is good, answer `200 OK` with the activation Moonbase should
make:

```json
{
  "trial": false,
  "productId": "my-plugin",
  "customerEmail": "jane@example.com",
  "activationMethod": "Online",
  "deviceSignature": "mbd2_Xq3...",
  "deviceName": "Studio Mac",
  "expiresAt": null
}
```

- **`deviceSignature` and `deviceName`: send back the ones you received.**
  Moonbase binds the license to the device your endpoint names, and the app
  accepts a license only when it is bound to its own device id. Name any other
  device, such as the id the earlier system used, and the license takes a seat
  but fails on the computer that asked for it.
- `productId`: the Moonbase product the verified license is for, as your
  endpoint maps the earlier system's products to Moonbase's. Never take it from
  the request: anyone can send any `productId`, and with auto-provisioning on, a
  key for one product would unlock another. Refuse a request whose `productId`
  is not the license's product, since the app accepts only a license of the
  product it is configured for anyway.
- `customerEmail` or `customerId`: who the license belongs to in Moonbase, by
  email or by Moonbase customer id. The customer must exist in Moonbase. A
  trial needs neither.
- `activationMethod`: `Online` for a license the app keeps validating with
  Moonbase. `Offline` makes a license that is never checked online again, and
  can't be deactivated.
- `trial` and `expiresAt`: `true` and the date the trial ends, in ISO 8601, to
  carry over a trial with the days it has left. Moonbase reads `expiresAt` only
  for a trial. A migrated license lasts as long as the customer's license in
  Moonbase does, and one auto-provisioned never ends, so import a time-limited
  license with its end date, or refuse it.

Answer with any status other than a 2xx to refuse the license. Moonbase refuses
the migration then, and the app treats the license as invalid. The status you
choose doesn't reach the app, so a refusal for good and a failure of your own
look the same to it: see [what can go wrong](#what-can-go-wrong) for how the
app retries.

A sketch of the endpoint, in TypeScript:

```ts
// The Moonbase product each of the earlier system's products became.
const moonbaseProducts: Record<string, string> = {
  'SOLSTICE-PRO': 'my-plugin',
}

app.post('/moonbase/validate-license', async (req, res) => {
  // Refuse every request while no key is configured, rather than match a
  // missing header against a missing key.
  const key = process.env.MOONBASE_MIGRATION_KEY
  if (!key || req.get('api-key') !== key)
    return res.sendStatus(401)

  const { license, productId, deviceSignature, deviceName } = req.body
  const verified = await earlierSystem.verify(license) // your own check
  if (!verified)
    return res.sendStatus(404)

  // The product the license is for, not the one the request names.
  const product = moonbaseProducts[verified.product]
  if (!product || product !== productId)
    return res.sendStatus(403)

  // Moonbase ends a trial at expiresAt, but not a license: refuse a
  // time-limited one, unless you imported it into Moonbase with its end date.
  if (!verified.trial && verified.expiresAt)
    return res.sendStatus(403)

  res.json({
    trial: verified.trial,
    productId: product,
    customerEmail: verified.email,
    activationMethod: 'Online',
    deviceSignature,
    deviceName,
    expiresAt: verified.trial ? verified.expiresAt : null,
  })
})
```

Moonbase derives an activation's id from the customer, the product and the
device. Migrating the same license on the same computer again therefore finds
the activation the first migration made rather than taking a second seat, which
is what makes it safe for the app to retry. Each computer a license migrates to
takes a seat of the customer's Moonbase license, as activating it would, so the
seat count still limits how far one license spreads.

## JUCE module

Give `ActivationConfig::findLicenseToMigrate` a function that returns the
license the earlier system left on this computer, or an empty string when there
is none:

```cpp
config.findLicenseToMigrate = []
{
    // Wherever the earlier licensing system kept it.
    const auto file = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("Helio Audio/Solstice/license.key");

    // Byte for byte: loadFileAsString() drops a byte order mark.
    juce::MemoryBlock bytes;
    if (! file.loadFileAsData(bytes))
        return juce::String();
    return juce::String::fromUTF8(static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize()));
};
```

When `start()` finds no license stored, it calls the function on a background
thread and migrates what it returns, while the overlay shows the loading screen.
A migrated license is stored and unlocks the plugin as an activated one does,
for every instance. Instances that start together migrate once: the first sends
the request, and the rest wait for it and load what it stored. They wait on a
lock of the migration's own, so the request doesn't hold up storing a license
elsewhere, such as an offline activation in another instance.

When a migration fails, the plugin opens on the welcome screen as on a new
install, and `onDiagnostic` says why. Other instances in the same process don't
try again, so a plugin loaded many times over doesn't send a request that would
fail the same way from each, or wait on each for a request to time out offline.
The next process to load the plugin, such as the next session in the host, tries
again. That is what picks up a fix: the network back, the customer imported into
Moonbase since, or your endpoint up again.

Once the module removes a license of its own (the customer deactivated this
computer, Moonbase revoked the license, or an offline license expired), it
never migrates again on this computer, since that would bring the license back.
It records that in a file beside the license file, named after it with
`.migration-settled` added.

The function returns text, and the license reaches your endpoint as the UTF-8
the function returned. Return it as the earlier system wrote it: your endpoint
may need it byte for byte, to check a signature over it, so don't trim it, and
read the file as bytes, as above. A license file that isn't UTF-8 text must be
encoded, with `juce::Base64::toBase64(bytes.getData(), bytes.getSize())` say,
and decoded by your endpoint.

## Core SDK

`licensing::migrate_license` exchanges the license and returns the Moonbase
license, without storing it:

```cpp
try {
    auto license = licensing.migrate_license(earlier_license_text);
    licensing.store().store_local_license(license);
} catch (const moonbase::license_invalid_error&) {
    // Refused: by your endpoint, or by Moonbase.
} catch (const moonbase::no_eligible_license_error&) {
    // The customer has no license of the product with a seat free.
} catch (const moonbase::api_error&) {
    // No answer, or no such customer or product in Moonbase (404). Try again later.
}
```

The license is a license like any other: store it, and validate it on startup
as you would an activated one.

## What can go wrong

| Exception | Cause |
| --- | --- |
| `license_invalid_error` | Your endpoint answered with anything but a 2xx, or Moonbase refused the license |
| `license_device_mismatch_error` | Your endpoint named another device than the `deviceSignature` it was sent. Moonbase activated the device it named, which takes a seat: fix the endpoint, and free that seat in Moonbase |
| `no_eligible_license_error` | The customer has no license of the product with a seat free: none was imported, or every seat is taken |
| `release_not_covered_error` | None of the customer's licenses covers this version of the app |
| `license_expired_error` | The license or trial has ended |
| `store_closed_error` | The store has closed |
| `api_error` with 404 | Moonbase has no customer with the email or id your endpoint answered, or no such product |
| `api_error` with 500 | License migration isn't turned on for the account (the message says "No suitable license migration found"), or your endpoint couldn't be reached or answered something Moonbase couldn't read |
| `api_error`, other | No answer from Moonbase: offline, rate limited, or something in between answered |
| `configuration_error` | The license is blank or not UTF-8 text; nothing was sent |

To try the whole path before shipping, point a development build at your
account with migration on, return a license from `findLicenseToMigrate` (or
call `migrate_license`), and check that the computer shows up under the
customer's license in Moonbase.
