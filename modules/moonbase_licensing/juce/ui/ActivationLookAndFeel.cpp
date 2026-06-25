#include "ActivationLookAndFeel.h"

namespace moonbase::juce_integration::icons {

static juce::String hexOf(juce::Colour c)
{
    return juce::String::formatted("#%02x%02x%02x", c.getRed(), c.getGreen(), c.getBlue());
}

std::unique_ptr<juce::Drawable> fromStroke(const juce::String& pathData,
                                           juce::Colour colour,
                                           float strokeWidth,
                                           float viewBox)
{
    juce::String svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "
        << juce::String(viewBox) << " " << juce::String(viewBox) << "\">"
        << "<path d=\"" << pathData << "\" fill=\"none\" stroke=\"" << hexOf(colour)
        << "\" stroke-width=\"" << juce::String(strokeWidth)
        << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>";

    if (auto xml = juce::XmlDocument::parse(svg))
        return juce::Drawable::createFromSVG(*xml);
    return nullptr;
}

std::unique_ptr<juce::Drawable> fromFill(const juce::String& pathData,
                                         juce::Colour colour,
                                         float viewBox)
{
    juce::String svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "
        << juce::String(viewBox) << " " << juce::String(viewBox) << "\">"
        << "<path d=\"" << pathData << "\" fill=\"" << hexOf(colour) << "\"/></svg>";

    if (auto xml = juce::XmlDocument::parse(svg))
        return juce::Drawable::createFromSVG(*xml);
    return nullptr;
}

// The Moonbase brand mark, verbatim from the marketing site's
// images/logos/moonbase.svg. Recoloured to a single colour at load time.
static const char* const kMoonbaseSvg = R"SVG(<svg viewBox="0 0 512 512" fill="none" xmlns="http://www.w3.org/2000/svg">
<g>
<path fill="FILLCOLOR" d="m493.17795,174.6135c6.257,75.3 -30.577,150.948 -100.579,190.758c-66.292,37.697 -145.001,33.405 -205.402,-4.415c0.409,-1.248 0.638,-2.577 0.638,-3.96c0,-7.026 -5.717,-12.743 -12.743,-12.743c-2.876,0 -5.524,0.969 -7.659,2.582c-18.813,-15.259 -35.159,-34.207 -47.856,-56.534c-54.663,-96.122 -21.054,-218.358 75.069,-273.021a201.218,201.218 0 0 1 33.302,-15.124c-32.428,3.667 -64.663,13.809 -94.827,30.962c-104.526,59.443 -149.511,183.241 -113.473,293.249c-3.376,3.057 -5.513,7.459 -5.513,12.362c0,9.207 7.491,16.698 16.698,16.698c0.136,0 0.267,-0.017 0.401,-0.02a252.205,252.205 0 0 0 8.683,16.691c67.869,119.344 219.635,161.073 338.98,93.204a250.416,250.416 0 0 0 32.893,-22.333c1.045,0.331 2.157,0.513 3.311,0.513c6.047,0 10.968,-4.921 10.968,-10.968c0,-0.605 -0.063,-1.196 -0.157,-1.775c69.545,-64.988 96.008,-165.054 67.266,-256.126m-446.526,25.112a5.071,5.071 0 0 1 -5.066,-5.066a5.072,5.072 0 0 1 5.066,-5.067a5.073,5.073 0 0 1 5.066,5.067a5.072,5.072 0 0 1 -5.066,5.066m-15.819,153.944c-8.238,0 -14.94,-6.702 -14.94,-14.94c0,-8.239 6.702,-14.941 14.94,-14.941c8.239,0 14.941,6.702 14.941,14.941c-0.001,8.238 -6.703,14.94 -14.941,14.94m144.258,-7.659c6.057,0 10.985,4.928 10.985,10.986c0,6.058 -4.928,10.985 -10.985,10.985c-6.058,0 -10.986,-4.928 -10.986,-10.985c0.001,-6.059 4.928,-10.986 10.986,-10.986m70.829,133.004c-5.079,0 -9.21,-4.132 -9.21,-9.21c0,-5.079 4.131,-9.21 9.21,-9.21c5.078,0 9.209,4.131 9.209,9.21c0,5.078 -4.13,9.21 -9.209,9.21m169.18,-37.289c-5.078,0 -9.209,-4.132 -9.209,-9.21s4.131,-9.209 9.209,-9.209s9.21,4.131 9.21,9.209s-4.132,9.21 -9.21,9.21"/>
<path fill="FILLCOLOR" d="m144.78195,275.9675c46.746,82.201 151.278,110.943 233.48,64.197c59.864,-34.044 91.363,-98.736 86.013,-163.13a170.094,170.094 0 0 0 -5.515,-31.052c-23.045,0.567 -63.864,10.137 -104.194,61.671c-52.045,66.5 -186.285,49.153 -228.855,17.372c3.503,17.443 9.796,34.632 19.071,50.942m233.513,-65.677a8.663,8.663 0 1 1 -8.663,8.663a8.662,8.662 0 0 1 8.663,-8.663m-37.006,62.618c7.02,0 12.712,5.691 12.712,12.712s-5.692,12.712 -12.712,12.712c-7.021,0 -12.712,-5.691 -12.712,-12.712s5.691,-12.712 12.712,-12.712m-102.544,12.712a6.78,6.78 0 1 1 0,13.56a6.78,6.78 0 0 1 0,-13.56"/>
<ellipse fill="FILLCOLOR" ry="7.3075" rx="7.3075" cy="152.05851" cx="251.33138"/>
<ellipse fill="FILLCOLOR" ry="5.35076" rx="5.35076" cy="210.31151" cx="204.89499"/>
<ellipse fill="FILLCOLOR" ry="12.09273" rx="12.09273" cy="200.15625" cx="302.28597"/>
</g>
</svg>)SVG";

std::unique_ptr<juce::Drawable> moonbaseMark(juce::Colour colour)
{
    const auto svg = juce::String(kMoonbaseSvg).replace("FILLCOLOR", hexOf(colour));
    if (auto xml = juce::XmlDocument::parse(svg))
        return juce::Drawable::createFromSVG(*xml);
    return nullptr;
}

// Heroicons-style 24x24 stroke paths, taken from the Solstice design.
const char* const offlineGlobe =
    "M9.348 14.652a3.75 3.75 0 010-5.304m5.304 0a3.75 3.75 0 010 5.304m-7.425 2.121a6.75 6.75 0 010-9.546"
    "m9.546 0a6.75 6.75 0 010 9.546M5.106 18.894c-3.808-3.807-3.808-9.98 0-13.788m13.788 0c3.808 3.807 "
    "3.808 9.98 0 13.788M12 12h.008v.008H12V12z";
const char* const back = "M15.75 19.5 8.25 12l7.5-7.5";
const char* const upload =
    "M19.5 14.25v-2.625a3.375 3.375 0 0 0-3.375-3.375h-1.5A1.125 1.125 0 0 1 13.5 7.125v-1.5a3.375 3.375 "
    "0 0 0-3.375-3.375H8.25m.75 12 3 3m0 0 3-3m-3 3v-6m-1.5-9H5.625c-.621 0-1.125.504-1.125 1.125v17.25c0 "
    ".621.504 1.125 1.125 1.125h12.75c.621 0 1.125-.504 1.125-1.125V11.25a9 9 0 0 0-9-9z";
const char* const fileDown = upload;
const char* const checkCircle = "M9 12.75 11.25 15 15 9.75M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0z";
const char* const fileQuestion =
    "M7.5 7.5h-.75A2.25 2.25 0 0 0 4.5 9.75v7.5a2.25 2.25 0 0 0 2.25 2.25h7.5a2.25 2.25 0 0 0 2.25-2.25v-7.5"
    "a2.25 2.25 0 0 0-2.25-2.25h-.75m-6 3.75 3-3m0 0 3 3m-3-3v11.25m6-2.25h.75a2.25 2.25 0 0 0 2.25-2.25v-7.5"
    "a2.25 2.25 0 0 0-2.25-2.25h-.75";
const char* const warning =
    "M12 9v3.75m-9.303 3.376c-.866 1.5.217 3.374 1.948 3.374h14.71c1.73 0 2.813-1.874 1.948-3.374L13.949 "
    "3.378c-.866-1.5-3.032-1.5-3.898 0L2.697 16.126zM12 15.75h.007v.008H12v-.008z";
const char* const lock =
    "M16.5 10.5V6.75a4.5 4.5 0 1 0-9 0v3.75m-.75 11.25h10.5a2.25 2.25 0 0 0 2.25-2.25v-6.75a2.25 2.25 0 0 "
    "0-2.25-2.25H6.75a2.25 2.25 0 0 0-2.25 2.25v6.75a2.25 2.25 0 0 0 2.25 2.25z";
const char* const externalLink =
    "M13.5 6H5.25A2.25 2.25 0 0 0 3 8.25v10.5A2.25 2.25 0 0 0 5.25 21h10.5A2.25 2.25 0 0 0 18 18.75V10.5"
    "m-10.5 6L21 3m0 0h-5.25M21 3v5.25";
const char* const monitor =
    "M9 17.25v1.007a3 3 0 0 1-.879 2.122L7.5 21h9l-.621-.621A3 3 0 0 1 15 18.257V17.25m6-12V15a2.25 2.25 0 "
    "0 1-2.25 2.25H5.25A2.25 2.25 0 0 1 3 15V5.25m18 0A2.25 2.25 0 0 0 18.75 3H5.25A2.25 2.25 0 0 0 3 5.25"
    "m18 0V12a2.25 2.25 0 0 1-2.25 2.25H5.25A2.25 2.25 0 0 1 3 12V5.25";
const char* const check = "M4.5 12.75l6 6 9-13.5";
const char* const cross = "M6 18 18 6M6 6l12 12";

} // namespace moonbase::juce_integration::icons
