#include <QApplication>
#include <QStyle>

#include <Windows.Foundation.h>
#include <systemmediatransportcontrolsinterop.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include "TaskbarComponentWin.h"
#include "PlayerComponent.h"
#include "player/AlbumArtProvider.h"
#include "input/InputComponent.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"

#include <robuffer.h>
#include <Windows.Storage.Streams.h>

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Media;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using ABI::Windows::Storage::Streams::IRandomAccessStreamReference;
using ABI::Windows::Storage::Streams::IRandomAccessStreamReferenceStatics;


/////////////////////////////////////////////////////////////////////////////////////////
TaskbarComponentWin::~TaskbarComponentWin()
{
  if (m_initialized)
  {
    m_systemControls->remove_ButtonPressed(m_buttonPressedToken);
    m_displayUpdater->ClearAll();
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setWindow(QQuickWindow* window)
{
  qDebug() << "Taskbar initialization started";
  TaskbarComponent::setWindow(window);

  bool EnableTaskbar = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "enableWindowsTaskbarIntegration").toBool();
  bool EnableMediaControls = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "enableWindowsMediaIntegration").toBool();

  if (EnableTaskbar) {
    m_button = new QWinTaskbarButton(m_window);
    m_button->setWindow(m_window);

    m_toolbar = new QWinThumbnailToolBar(m_window);
    m_toolbar->setWindow(m_window);

    m_prev = new QWinThumbnailToolButton(m_toolbar);
    connect(m_prev, &QWinThumbnailToolButton::clicked, this, &TaskbarComponentWin::onPrevClicked);

    m_pause = new QWinThumbnailToolButton(m_toolbar);
    connect(m_pause, &QWinThumbnailToolButton::clicked, this, &TaskbarComponentWin::onPauseClicked);

    m_next = new QWinThumbnailToolButton(m_toolbar);
    connect(m_next, &QWinThumbnailToolButton::clicked, this, &TaskbarComponentWin::onNextClicked);

    m_prev->setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_next->setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaSkipForward));

    m_toolbar->addButton(m_prev);
    m_toolbar->addButton(m_pause);
    m_toolbar->addButton(m_next);
  }

  connect(&PlayerComponent::Get(), &PlayerComponent::positionUpdate, this, &TaskbarComponentWin::setProgress);
  connect(&PlayerComponent::Get(), &PlayerComponent::playing, this, &TaskbarComponentWin::playing);
  connect(&PlayerComponent::Get(), &PlayerComponent::paused, this, &TaskbarComponentWin::paused);
  connect(&PlayerComponent::Get(), &PlayerComponent::stopped, this, &TaskbarComponentWin::stopped);
  connect(&PlayerComponent::Get(), &PlayerComponent::onMetaData, this, &TaskbarComponentWin::onMetaData);
  connect(&PlayerComponent::Get(), &PlayerComponent::metadataChanged, this, [this](const QVariantMap& meta) {
    onMetaData(meta, QUrl());
  });

  if (PlayerComponent::Get().albumArtProvider())
  {
    connect(PlayerComponent::Get().albumArtProvider(), &AlbumArtProvider::artworkReady,
            this, &TaskbarComponentWin::onAlbumArtReady);
  }

  setControlsVisible(false);
  setPaused(false);

  if (EnableMediaControls) {
    initializeMediaTransport((HWND)window->winId());
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::onPauseClicked()
{
  InputComponent::Get().sendAction("play_pause");
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::onNextClicked()
{
  InputComponent::Get().sendAction("next");
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::onPrevClicked()
{
  InputComponent::Get().sendAction("previous");
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::playing()
{
  setControlsVisible(true);
  setPaused(false);
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::paused()
{
  setPaused(true);
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::stopped()
{
  setControlsVisible(false);
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setControlsVisible(bool value)
{
  if (m_button) {
    m_button->progress()->setVisible(value);

    for (auto& button : m_toolbar->buttons())
    {
      button->setVisible(value);
    }
  }

  if (m_initialized)
  {
    m_systemControls->put_PlaybackStatus(MediaPlaybackStatus::MediaPlaybackStatus_Stopped);
    m_systemControls->put_IsEnabled(value);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setProgress(quint64 value)
{
  if (m_button) {
    qint64 duration = PlayerComponent::Get().getDuration();
    int progress = 0;
    if (duration != 0) {
      progress = (int) (value / duration / 10);
    }
    m_button->progress()->setValue(progress);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setPaused(bool value)
{
  if (m_button) {
    if (value)
    {
      // m_pause->setToolTip("Resume");
      m_pause->setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaPlay));
    }
    else
    {
      // m_pause->setToolTip("Pause");
      m_pause->setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaPause));
    }

    m_button->progress()->setPaused(value);
  }

  if (m_initialized)
  {
    auto status = value ? MediaPlaybackStatus::MediaPlaybackStatus_Paused : MediaPlaybackStatus::MediaPlaybackStatus_Playing;
    m_systemControls->put_PlaybackStatus(status);
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::initializeMediaTransport(HWND hwnd)
{
  ComPtr<ISystemMediaTransportControlsInterop> interop;
  auto hr = GetActivationFactory(HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(), &interop);
  if (FAILED(hr))
  {
    qWarning() << "Failed instantiating interop object";
    return;
  }

  hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&m_systemControls));
  if (FAILED(hr))
  {
    qWarning() << "Failed to GetForWindow";
    return;
  }

  auto handler = Callback<
    ITypedEventHandler<
      SystemMediaTransportControls*,
      SystemMediaTransportControlsButtonPressedEventArgs*>>(
    [this](ISystemMediaTransportControls* sender, ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
      return buttonPressed(sender, args);
    });
  hr = m_systemControls->add_ButtonPressed(handler.Get(), &m_buttonPressedToken);
  if (FAILED(hr))
  {
    qWarning() << "Failed to add callback handler";
    return;
  }

  hr = m_systemControls->put_IsEnabled(false);

  hr = m_systemControls->put_IsPlayEnabled(true);
  hr = m_systemControls->put_IsPauseEnabled(true);
  hr = m_systemControls->put_IsPreviousEnabled(true);
  hr = m_systemControls->put_IsNextEnabled(true);
  hr = m_systemControls->put_IsStopEnabled(true);
  hr = m_systemControls->put_IsRewindEnabled(true);
  hr = m_systemControls->put_IsFastForwardEnabled(true);
  hr = m_systemControls->put_IsChannelDownEnabled(true);
  hr = m_systemControls->put_IsChannelUpEnabled(true);

  hr = m_systemControls->get_DisplayUpdater(&m_displayUpdater);
  if (FAILED(hr))
  {
    qWarning() << "Failed to get Display updater";
    return;
  }

  m_displayUpdater->put_AppMediaId(HStringReference(L"org.jellyfin.JellyfinDesktop").Get());

  m_initialized = true;
  qInfo() << "SystemMediaTransportControls successfully initialized";
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::onMetaData(const QVariantMap& meta, QUrl baseUrl)
{
  if (!m_initialized)
    return;

  HRESULT hr;
  auto mediaType = meta["MediaType"].toString();

  hr = m_displayUpdater->ClearAll();
  if (FAILED(hr))
  {
    qWarning() << "Failed to clear display metadata";
    return;
  }

  // Re-set AppMediaId after ClearAll wipes it
  m_displayUpdater->put_AppMediaId(HStringReference(L"org.jellyfin.JellyfinDesktop").Get());

  if (mediaType == "Video")
  {
    setVideoMeta(meta);
  }
  else // if (mediaType == "Audio") most likely
  {
    setAudioMeta(meta);
  }

  hr = m_displayUpdater->Update();
  if (FAILED(hr))
  {
    qWarning() << "Failed to update the display";
    return;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setAudioMeta(const QVariantMap& meta)
{
  HRESULT hr;

  hr = m_displayUpdater->put_Type(MediaPlaybackType::MediaPlaybackType_Music);
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the media type to music";
    return;
  }

  ComPtr<IMusicDisplayProperties> musicProps;
  hr = m_displayUpdater->get_MusicProperties(musicProps.GetAddressOf());
  if (FAILED(hr))
  {
    qWarning() << "Failed to get music properties";
    return;
  }

  auto artist = meta["Artists"].toStringList().join(", ");
  hr = musicProps->put_Artist(HStringReference((const wchar_t*)artist.utf16()).Get());
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the music's artist";
    return;
  }

  auto title = meta["Name"].toString();
  hr = musicProps->put_Title(HStringReference((const wchar_t*)title.utf16()).Get());
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the music's title";
    return;
  }

  auto albumArtist = meta["AlbumArtist"].toString();
  hr = musicProps->put_AlbumArtist(HStringReference((const wchar_t*)albumArtist.utf16()).Get());
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the music's album artist";
    return;
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::setVideoMeta(const QVariantMap& meta)
{
  HRESULT hr;

  hr = m_displayUpdater->put_Type(MediaPlaybackType::MediaPlaybackType_Video);
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the media type to video";
    return;
  }

  ComPtr<IVideoDisplayProperties> videoProps;
  hr = m_displayUpdater->get_VideoProperties(videoProps.GetAddressOf());
  if (FAILED(hr))
  {
    qWarning() << "Failed to get video properties";
    return;
  }

  auto title = meta["Name"].toString();
  hr = videoProps->put_Title(HStringReference((const wchar_t*)title.utf16()).Get());
  if (FAILED(hr))
  {
    qWarning() << "Failed to set the video title";
    return;
  }

  if (meta["Type"] == "Episode")
  {
    auto subtitle = meta["SeriesName"].toString();
    hr = videoProps->put_Subtitle(HStringReference((const wchar_t*)subtitle.utf16()).Get());
    if (FAILED(hr))
    {
      qWarning() << "Failed to set the video subtitle";
      return;
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void TaskbarComponentWin::onAlbumArtReady(const QByteArray& imageData)
{
  if (!m_initialized || !m_displayUpdater)
    return;

  HRESULT hr;

  // Create an InMemoryRandomAccessStream and write the image data into it.
  ComPtr<ABI::Windows::Storage::Streams::IRandomAccessStream> memStream;
  hr = ActivateInstance(HStringReference(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream).Get(),
                        &memStream);
  if (FAILED(hr))
  {
    qWarning() << "Failed to create InMemoryRandomAccessStream";
    return;
  }

  // Get the IOutputStream interface to write data.
  ComPtr<ABI::Windows::Storage::Streams::IOutputStream> outputStream;
  hr = memStream.As(&outputStream);
  if (FAILED(hr))
  {
    qWarning() << "Failed to get IOutputStream from memory stream";
    return;
  }

  // Create a DataWriter to write bytes into the stream.
  ComPtr<ABI::Windows::Storage::Streams::IDataWriter> writer;
  hr = ActivateInstance(HStringReference(RuntimeClass_Windows_Storage_Streams_DataWriter).Get(), &writer);
  if (FAILED(hr))
  {
    qWarning() << "Failed to create DataWriter";
    return;
  }

  // Attach the writer to the output stream.
  ComPtr<ABI::Windows::Storage::Streams::IDataWriterFactory> writerFactory;
  hr = GetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_DataWriter).Get(), &writerFactory);
  if (FAILED(hr))
  {
    qWarning() << "Failed to get DataWriter factory";
    return;
  }

  hr = writerFactory->CreateDataWriter(outputStream.Get(), &writer);
  if (FAILED(hr))
  {
    qWarning() << "Failed to create DataWriter with output stream";
    return;
  }

  // Write the image bytes.
  hr = writer->WriteBytes(imageData.size(), (BYTE*)imageData.constData());
  if (FAILED(hr))
  {
    qWarning() << "Failed to write image bytes";
    return;
  }

  // Flush the writer (synchronously store to stream).
  ComPtr<ABI::Windows::Foundation::IAsyncOperation<UINT32>> storeOp;
  hr = writer->StoreAsync(&storeOp);
  if (FAILED(hr))
  {
    qWarning() << "Failed to store DataWriter";
    return;
  }

  // Despite the name, this "async" operation is entirely in-memory.
  ComPtr<ABI::Windows::Foundation::IAsyncInfo> asyncInfo;
  hr = storeOp.As(&asyncInfo);
  if (SUCCEEDED(hr))
  {
    ABI::Windows::Foundation::AsyncStatus status;
    do {
      hr = asyncInfo->get_Status(&status);
    } while (SUCCEEDED(hr) && status == ABI::Windows::Foundation::AsyncStatus::Started);

    if (status != ABI::Windows::Foundation::AsyncStatus::Completed)
    {
      qWarning() << "DataWriter store did not complete successfully";
      return;
    }
  }

  UINT32 bytesWritten = 0;
  storeOp->GetResults(&bytesWritten);

  // Detach the stream from the writer so it doesn't get closed.
  hr = writer->DetachStream(&outputStream);

  // Seek back to the beginning.
  hr = memStream->Seek(0);
  if (FAILED(hr))
  {
    qWarning() << "Failed to seek memory stream to beginning";
    return;
  }

  // Create a RandomAccessStreamReference from the in-memory stream.
  ComPtr<IRandomAccessStreamReferenceStatics> streamRefFactory;
  hr = GetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(),
    &streamRefFactory);
  if (FAILED(hr))
  {
    qWarning() << "Failed to get RandomAccessStreamReference factory";
    return;
  }

  hr = streamRefFactory->CreateFromStream(memStream.Get(), &m_thumbnail);
  if (FAILED(hr))
  {
    qWarning() << "Failed to create stream reference from memory stream";
    return;
  }

  hr = m_displayUpdater->put_Thumbnail(m_thumbnail.Get());
  if (FAILED(hr))
  {
    qWarning() << "Failed to set thumbnail";
    return;
  }

  hr = m_displayUpdater->Update();
  if (FAILED(hr))
  {
    qWarning() << "Failed to update display after setting thumbnail";
    return;
  }

  qDebug() << "Taskbar thumbnail updated from album art (" << bytesWritten << "bytes)";
}

/////////////////////////////////////////////////////////////////////////////////////////
HRESULT TaskbarComponentWin::buttonPressed(ISystemMediaTransportControls* sender,
  ISystemMediaTransportControlsButtonPressedEventArgs* args)
{
  SystemMediaTransportControlsButton button;
  auto hr = args->get_Button(&button);
  if (FAILED(hr))
  {
    qWarning() << "Failed to get the pressed button";
    return hr;
  }

  switch (button)
  {
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Play:
      InputComponent::Get().sendAction("play_pause");
      qDebug() << "Received play button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Pause:
      InputComponent::Get().sendAction("play_pause");
      qDebug() << "Received pause button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Next:
      InputComponent::Get().sendAction("next");
      qDebug() << "Received next button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Previous:
      InputComponent::Get().sendAction("previous");
      qDebug() << "Received previous button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Stop:
      InputComponent::Get().sendAction("stop");
      qDebug() << "Received stop button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_FastForward:
      InputComponent::Get().sendAction("seek_forward");
      qDebug() << "Received seek_forward button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Rewind:
      InputComponent::Get().sendAction("seek_backward");
      qDebug() << "Received seek_backward button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_ChannelUp:
      InputComponent::Get().sendAction("channelup");
      qDebug() << "Received channelup button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_ChannelDown:
      InputComponent::Get().sendAction("channeldown");
      qDebug() << "Received channeldown button press";
      break;
    case SystemMediaTransportControlsButton::SystemMediaTransportControlsButton_Record:
      qDebug() << "Received unsupported button press";
      break;
  }

  return S_OK;
}
