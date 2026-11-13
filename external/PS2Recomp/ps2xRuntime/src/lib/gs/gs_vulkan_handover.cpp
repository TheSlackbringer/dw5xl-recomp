// SPDX-License-Identifier: GPL-3.0-only
#include "gs_vulkan_handover.h"

#include <algorithm>
#include <utility>

namespace
{
std::atomic<uint64_t> g_nextImageId{1};
}

VulkanGsHandover::VulkanGsHandover(std::shared_ptr<void> renderer, uint32_t places)
    : m_renderer(std::move(renderer)), m_rules(places), m_places(places)
{
}

HandedPicture VulkanGsHandover::takeNewest(uint64_t maxField)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t superseded = 0;
    const uint32_t place = m_rules.takeNewest(maxField, superseded);
    m_counts.superseded += superseded;
    if (place == ps2x::gs::HandoverPlaces::kNone)
        return {};
    ++m_counts.taken;
    return std::move(m_places[place].picture);
}

void VulkanGsHandover::release(HandedPicture &picture)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Only the picture that holds the place gives it back (a stale copy of an earlier picture does nothing).
        if (picture.place < m_places.size() && m_rules.state(picture.place) == ps2x::gs::HandoverPlaces::State::Held &&
            m_places[picture.place].imageId == picture.imageId)
            m_rules.release(picture.place);
    }
    picture = {};
}

VulkanGsHandover::Counts VulkanGsHandover::counts() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_counts;
}

VulkanGsHandover::Target VulkanGsHandover::begin(Vulkan::Device &device, uint32_t width, uint32_t height)
{
    Target target;
    Vulkan::ImageHandle image;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool gaveUp = false;
        target.place = m_rules.forNewPicture(gaveUp);
        if (target.place == ps2x::gs::HandoverPlaces::kNone)
        {
            ++m_counts.noPlace;
            return {};
        }
        if (gaveUp)
        {
            ++m_counts.givenUp;
            m_places[target.place].picture = {};
        }
        image = m_places[target.place].image;
        target.imageId = m_places[target.place].imageId;
    }
    // The place is being written from here on: only this thread touches it until finish() or abandon(). The image
    // is made without the hand-over's lock, so that the presenter does not wait for it.
    if (!image || image->get_width() != width || image->get_height() != height)
    {
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image = device.create_image(info);
        target.imageId = image ? g_nextImageId.fetch_add(1u, std::memory_order_relaxed) : 0u;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!image)
        {
            m_rules.abandon(target.place);
            return {};
        }
        device.set_name(*image, "Handed picture");
        m_places[target.place].image = image;
        m_places[target.place].imageId = target.imageId;
        ++m_counts.images;
    }
    target.image = std::move(image);
    return target;
}

void VulkanGsHandover::finish(const Target &target, HandedPicture picture, bool alsoRead)
{
    picture.renderer = m_renderer;
    picture.image = target.image;
    picture.imageId = target.imageId;
    picture.place = target.place;
    picture.width = target.image->get_width();
    picture.height = target.image->get_height();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rules.ready(target.place, picture.field);
    m_places[target.place].picture = std::move(picture);
    ++m_counts.handed;
    m_counts.alsoRead += alsoRead ? 1u : 0u;
}

void VulkanGsHandover::abandon(const Target &target)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rules.abandon(target.place);
}

void VulkanGsHandover::forget()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_counts.givenUp += m_rules.giveUpReady();
    for (uint32_t i = 0; i < m_places.size(); ++i)
    {
        m_places[i].picture = {};
        // A held picture's image stays in its place: the next picture for the place gets a new one if the size
        // differs. Whoever holds the picture keeps the image alive in any case.
        if (m_rules.state(i) == ps2x::gs::HandoverPlaces::State::Free)
        {
            m_places[i].image.reset();
            m_places[i].imageId = 0;
        }
    }
}

HandedPictureViews::HandedPictureViews(Vulkan::Device &presenter, uint32_t keepFrames)
    : m_device(presenter), m_keepFrames(std::max<uint32_t>(keepFrames, 2u))
{
}

const Vulkan::ImageView *HandedPictureViews::view(const HandedPicture &picture)
{
    if (!picture)
        return nullptr;
    for (Entry &entry : m_entries)
    {
        if (entry.imageId == picture.imageId)
        {
            entry.usedFrame = m_frame;
            return entry.view.get();
        }
    }
    Entry entry;
    entry.renderer = picture.renderer;
    entry.source = picture.image;
    entry.imageId = picture.imageId;
    entry.usedFrame = m_frame;
    const Vulkan::ImageCreateInfo &info = picture.image->get_create_info();
    entry.wrapped = m_device.wrap_image(info, picture.image->get_image());
    if (!entry.wrapped)
        return nullptr;
    Vulkan::ImageViewCreateInfo view;
    view.image = entry.wrapped.get();
    view.format = info.format;
    view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    view.levels = 1;
    view.layers = 1;
    entry.view = m_device.create_image_view(view);
    if (!entry.view)
        return nullptr;
    m_entries.push_back(std::move(entry));
    return m_entries.back().view.get();
}

void HandedPictureViews::nextFrame()
{
    ++m_frame;
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [this](const Entry &entry) { return entry.usedFrame + m_keepFrames < m_frame; }),
                    m_entries.end());
}
