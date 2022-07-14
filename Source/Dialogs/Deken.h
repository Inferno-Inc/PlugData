#pragma once
#include "../Utility/JSON.h"

struct Spinner : public Component, public Timer
{
    bool isSpinning = false;
    
    void startSpinning()
    {
        setVisible(true);
        startTimer(20);
    }
    
    void stopSpinning()
    {
        setVisible(false);
        stopTimer();
    }
    
    void timerCallback() override
    {
        repaint();
    }
    
    void paint(Graphics& g) override
    {
        getLookAndFeel().drawSpinningWaitAnimation(g, findColour(PlugDataColour::textColourId), 3, 3, getWidth() - 6, getHeight() - 6);
    }
};

// Struct with info about the deken package
struct PackageInfo
{
    PackageInfo(String name, String author, String timestamp, String url, String description, String version, StringArray objects)
    {
        this->name = name;
        this->author = author;
        this->timestamp = timestamp;
        this->url = url;
        this->description = description;
        this->version = version;
        this->objects = objects;
        packageId = Base64::toBase64(name + "_" + version + "_" + timestamp + "_" + author);
        
    }
    
    // fast compare by ID
    friend bool operator==(const PackageInfo& lhs, const PackageInfo& rhs)
    {
        return lhs.packageId == rhs.packageId;
    }
    
    String name, author, timestamp, url, description, version, packageId;
    StringArray objects;
};

// Array with package info to store the result of a search action in
using PackageList = Array<PackageInfo>;

using namespace nlohmann;

struct PackageManager : public Thread, public ActionBroadcaster, public ValueTree::Listener, public DeletedAtShutdown
{
    struct PackageSorter
    {
        static void sort(ValueTree& packageState) {
            PackageSorter sorter;
            packageState.sort(sorter, nullptr, true);
        }
        
        static int compareElements (const ValueTree& first, const ValueTree& second) {
            return first.getType().toString().compare(second.getType().toString());
        }
    };
    
    
    struct DownloadTask : public Thread
    {
        PackageManager& manager;
        PackageInfo packageInfo;
        
        std::unique_ptr<InputStream> instream;
        
        DownloadTask(PackageManager& m, PackageInfo& info)
        :
        Thread("Download Thread"),
        manager(m),
        packageInfo(info)
        {
            int statusCode = 0;
            instream = URL(info.url).createInputStream(URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                                                                                  .withConnectionTimeoutMs (5000)
                                                       .withStatusCode(&statusCode));
            
            if (instream != nullptr && statusCode == 200)
            {
                startThread(3);
            }
            else {
                finish(Result::fail("Failed to start download"));
                return;
            }
        };
        
        ~DownloadTask() {
            stopThread(-1);
        }
        
        void run() override {
            MemoryBlock dekData;
            
            int64 totalBytes = instream->getTotalLength();
            int64 bytesDownloaded = 0;
            
            MemoryOutputStream mo (dekData, true);

            while(true)
            {
                if(threadShouldExit()) {
                    finish(Result::fail("Download cancelled"));
                    return;
                }

                auto written = mo.writeFromInputStream (*instream, 8192);

                if(written == 0)
                    break;

                bytesDownloaded += written;
                
                float progress = static_cast<long double>(bytesDownloaded) / static_cast<long double>(totalBytes);
                
                MessageManager::callAsync([this, progress]() mutable {
                    onProgress(progress);
                });
            }
            
            MemoryInputStream input (dekData, false);
            ZipFile zip (input);
            
            if (zip.getNumEntries() == 0)
                return Result::fail ("The downloaded file was not a valid Deken package");
            
            auto extractedPath = filesystem.getChildFile(packageInfo.name).getFullPathName();
            auto result = zip.uncompressTo(filesystem);
            
            if(!result.wasOk()) {
                finish(result);
                return;
            }
            
            // Tell deken about the newly installed package
            manager.addPackageToRegister(packageInfo, extractedPath);
            
            finish(Result::ok());
        }
        
        void finish(Result result) {
            MessageManager::callAsync(
                                      [this, result]() mutable
                                      {
                                          onFinish(result);
                                          waitForThreadToExit(-1);
                                          
                                          // self-destruct
                                          manager.downloads.removeObject(this);
              
                                      });
        }
                
        std::function<void(float)> onProgress;
        std::function<void(Result)> onFinish;
        bool isFinished = false;
    };
    
    PackageManager() : Thread("Deken thread")
    {
        if (!filesystem.exists())
        {
            filesystem.createDirectory();
        }
        
        if (pkgInfo.existsAsFile())
        {
            auto newTree = ValueTree::fromXml(pkgInfo.loadFileAsString());
            if (newTree.isValid() && newTree.getType() == Identifier("pkg_info"))
            {
                packageState = newTree;
            }
            else {
                packageState = ValueTree("pkg_info");
            }
        }
        else {
            packageState = ValueTree("pkg_info");
        }
        
        packageState.addListener(this);
        PackageSorter::sort(packageState);
        
        sendActionMessage("");
        startThread(3);
    }
    
    ~PackageManager() {
        if(webstream) webstream->cancel();
        downloads.clear();
        stopThread(-1);
    }
    
    void update() {
        sendActionMessage("");
        startThread(3);
    }
    
    void run() override
    {
        // Continue on pipe errors
#ifndef _MSC_VER
        signal(SIGPIPE, SIG_IGN);
#endif
        allPackages = getAvailablePackages();
        sendActionMessage("");
    }
    
    StringArray getObjectInfo(const String& objectUrl) {
        
        StringArray result;
        
        webstream = std::make_unique<WebInputStream>(URL("https://deken.puredata.info/info.json?url=" + objectUrl), false);
        webstream->connect(nullptr);
        
        if(webstream->isError()) {
            sendActionMessage("Failed to connect to Deken server");
            return {};
        }
        
        // Read json result
        auto json = webstream->readString();
        
        if(json.isEmpty()) {
            sendActionMessage("Invalid response from Deken server");
            return {};
        }
        
        try {
            // Parse outer JSON layer
            auto parsedJson = json::parse(json.toStdString());
            
            // Read json
            auto objects = (*((*(parsedJson["result"]["libraries"].begin())).begin())).at(0)["objects"];
            
            for(auto obj : objects) {
                result.add(obj["name"]);
            }
        } catch (json::parse_error& e) {
            sendActionMessage("Invalid response from Deken server");
            return {};
        }
        
        return result;
    }
    
    PackageList getAvailablePackages()
    {
        PackageList packages;
        
        webstream = std::make_unique<WebInputStream>(URL("https://deken.puredata.info/search.json"), false);
        webstream->connect(nullptr);
        
        if(webstream->isError()) {
            sendActionMessage("Failed to connect to Deken server");
            return {};
        }
        
        // Read json result
        auto json = webstream->readString();
        
        if(json.isEmpty()) {
            sendActionMessage("Invalid response from Deken server");
            return {};
        }
        
        
        if(threadShouldExit()) return {};
        
        // In case the JSON is invalid
        try
        {
            // JUCE json parsing unfortunately fails to parse deken's json...
            auto parsedJson = json::parse(json.toStdString());
            
            // Read json
            auto object = parsedJson["result"]["libraries"];
            
            // Valid result, go through the options
            for (const auto versions : object)
            {
                PackageList results;
                
                if(threadShouldExit()) return {};
                
                // Loop through the different versions
                for (auto v : versions)
                {
                    // Loop through architectures
                    for (auto arch : v)
                    {
                        auto archs = arch["archs"];
                        // Look for matching platform
                        String platform = archs[0].is_null() ? "" : archs[0];
                        
                        if (checkArchitecture(platform))
                        {
                            // Extract info
                            String name = arch["name"];
                            String author = arch["author"];
                            String timestamp = arch["timestamp"];
                            
                            String description = arch["description"];
                            String url = arch["url"];
                            String version = arch["version"];
                            
                            StringArray objects = getObjectInfo(url);
                            
                            // Add valid option
                            results.add({name, author, timestamp, url, description, version, objects});
                        }
                    }
                }
                
                if (!results.isEmpty())
                {
                    // Sort by alphabetically by timestamp to get latest version
                    // The timestamp format is yyyy:mm::dd hh::mm::ss so this should work
                    std::sort(results.begin(), results.end(), [](const auto& result1, const auto& result2) { return result1.timestamp.compare(result2.timestamp) > 0; });
                    
                    auto info = results.getReference(0);
                    packages.addIfNotAlreadyThere(info);
                }
            }
        }
        catch (json::parse_error& e)
        {
            sendActionMessage("Invalid response from Deken server");
        }
        
        return packages;
    }
    
    static bool checkArchitecture(String platform)
    {
        // Check OS
        if (platform.upToFirstOccurrenceOf("-", false, false) != os) return false;
        platform = platform.fromFirstOccurrenceOf("-", false, false);
        
        // Check floatsize
        if (platform.fromLastOccurrenceOf("-", false, false) != floatsize) return false;
        platform = platform.upToLastOccurrenceOf("-", false, false);
        
        if (machine.contains(platform)) return true;
        
        return false;
    }
    
    // When a property in our pkginfo changes, save it immediately
    void valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property) override
    {
        pkgInfo.replaceWithText(packageState.toXmlString());
    }
    
    void valueTreeChildAdded(ValueTree& parentTree, ValueTree& childWhichHasBeenAdded) override
    {
        pkgInfo.replaceWithText(packageState.toXmlString());
    }
    
    void valueTreeChildRemoved(ValueTree& parentTree, ValueTree& childWhichHasBeenRemoved, int indexFromWhichChildWasRemoved) override
    {
        pkgInfo.replaceWithText(packageState.toXmlString());
    }
    
    void uninstall(PackageInfo& packageInfo)
    {
        auto toRemove = packageState.getChildWithProperty("ID", packageInfo.packageId);
        if (toRemove.isValid())
        {
            auto folder = File(toRemove.getProperty("Path").toString());
            folder.deleteRecursively();
            packageState.removeChild(toRemove, nullptr);
        }
    }
    
    DownloadTask* install(PackageInfo packageInfo)
    {
        // Make sure https is used
        packageInfo.url = packageInfo.url.replaceFirstOccurrenceOf("http://", "https://");

        // Download file and return unique ptr to task object
        return downloads.add(new DownloadTask(*this, packageInfo));
    }
    
    void addPackageToRegister(const PackageInfo& info, String path)
    {
        ValueTree pkgEntry = ValueTree(info.name);
        pkgEntry.setProperty("ID", info.packageId, nullptr);
        pkgEntry.setProperty("Author", info.author, nullptr);
        pkgEntry.setProperty("Timestamp", info.timestamp, nullptr);
        pkgEntry.setProperty("Description", info.description, nullptr);
        pkgEntry.setProperty("Version", info.version, nullptr);
        pkgEntry.setProperty("Path", path, nullptr);
        pkgEntry.setProperty("URL", info.url, nullptr);
        
        // Prevent duplicate entries
        if (packageState.getChildWithProperty("ID", info.packageId).isValid())
        {
            packageState.removeChild(packageState.getChildWithProperty("ID", info.packageId), nullptr);
        }
        packageState.appendChild(pkgEntry, nullptr);
        
        PackageSorter::sort(packageState);
    }
    
    
    bool packageExists(const PackageInfo& info)
    {
        return packageState.getChildWithProperty("ID", info.packageId).isValid();
    }
    
    // Checks if the current package is already being downloaded
    DownloadTask* getDownloadForPackage(PackageInfo& info)
    {
        for (auto* download : downloads)
        {
            if (download->packageInfo == info)
            {
                return download;
            }
        }
        
        return nullptr;
    }

    
    PackageList allPackages;
    
    inline static File filesystem = File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getChildFile("PlugData").getChildFile("Library").getChildFile("Deken");
    
    // Package info file
    File pkgInfo = filesystem.getChildFile(".pkg_info");
    
    // Package state tree, keeps track of which packages are installed and saves it to pkgInfo
    ValueTree packageState = ValueTree("pkg_info");
    
    // Thread for unzipping and installing packages
    OwnedArray<DownloadTask> downloads;
    
    std::unique_ptr<WebInputStream> webstream;
    
    static inline const String floatsize = String(PD_FLOATSIZE);
    static inline const String os =
#if JUCE_LINUX
    "Linux"
#elif JUCE_MAC
    "Darwin"
#elif JUCE_WINDOWS
    "Windows"
    // PlugData has no official BSD support and testing, but for completeness:
#elif defined __FreeBSD__
    "FreeBSD"
#elif defined __NetBSD__
    "NetBSD"
#elif defined __OpenBSD__
    "OpenBSD"
#else
#if defined(__GNUC__)
#warning unknown OS
#endif
    0
#endif
    ;
    
    static inline const StringArray machine =
#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
    {"amd64", "x86_64"}
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__) || defined(_M_IX86)
    {"i386", "i686", "i586"}
#elif defined(__ppc__)
    {"ppc", "PowerPC"}
#elif defined(__aarch64__)
    {"arm64"}
#elif __ARM_ARCH == 6 || defined(__ARM_ARCH_6__)
    { "armv6", "armv6l", "arm" }
#elif __ARM_ARCH == 7 || defined(__ARM_ARCH_7__)
    {"armv7l", "armv7", "armv6l", "armv6", "arm"}
#else
#if defined(__GNUC__)
#warning unknown architecture
#endif
    {}
#endif
    ;
    
    
    // Create a single package manager that exists even when the dialog is not open
    // This allows more efficient pre-fetching of packages, and also makes it easy to
    // continue downloading when the dialog closes
    // Inherits from deletedAtShutdown to handle cleaning up
    JUCE_DECLARE_SINGLETON (PackageManager, false)
};

JUCE_IMPLEMENT_SINGLETON (PackageManager)

class Deken : public Component, public ListBoxModel, public ScrollBar::Listener, public ActionListener
{
    
public:
    Deken()
    {
        setInterceptsMouseClicks(false, true);
        
        listBox.setModel(this);
        listBox.setRowHeight(32);
        listBox.setOutlineThickness(0);
        listBox.deselectAllRows();
        listBox.getViewport()->setScrollBarsShown(true, false, false, false);
        listBox.addMouseListener(this, true);
        listBox.setColour(ListBox::backgroundColourId, Colours::transparentBlack);
        listBox.getViewport()->getVerticalScrollBar().addListener(this);
        
        input.setJustification(Justification::centredLeft);
        input.setBorder({1, 23, 3, 1});
        input.setName("sidebar::searcheditor");
        input.onTextChange = [this]() {
            filterResults();
        };
        
        clearButton.setName("statusbar:clearsearch");
        clearButton.setAlwaysOnTop(true);
        clearButton.onClick = [this]()
        {
            input.clear();
            input.giveAwayKeyboardFocus();
            input.repaint();
            filterResults();
        };
        
        updateSpinner.setAlwaysOnTop(true);
    
        addAndMakeVisible(clearButton);
        addAndMakeVisible(listBox);
        addAndMakeVisible(input);
        addAndMakeVisible(updateSpinner);
        
    
        refreshButton.setTooltip("Refresh packages");
        refreshButton.setName("statusbar:refresh");
        addAndMakeVisible(refreshButton);
        refreshButton.setConnectedEdges(12);
        refreshButton.onClick = [this]()
        {
            packageManager->startThread(3);
            packageManager->sendActionMessage("");
        };
        
        if(packageManager->isThreadRunning()) {
            input.setEnabled(false);
            input.setText("Updating Packages...");
            updateSpinner.startSpinning();
        }
        else {
            updateSpinner.setVisible(false);
        }
        
        packageManager->addActionListener(this);
        filterResults();
    }
    
    ~Deken() {
        packageManager->removeActionListener(this);
    }
    
    // Package update starts
    void actionListenerCallback (const String &message) override {
        
        auto* thread = dynamic_cast<Thread*>(packageManager);
        bool running = thread->isThreadRunning();
        
        // Handle errors
        if(message.isNotEmpty()) {
            showError(message);
            input.setEnabled(false);
            updateSpinner.stopSpinning();
            return;
        }
        
        if(running) {
            
            input.setText("Updating packages...");
            input.setEnabled(false);
            updateSpinner.startSpinning();
        }
        else {
            // Clear text if it was previously disabled
            // If it wasn't, this is just an update call from the package manager
            if(!input.isEnabled()) {
                input.setText("");
            }
           
            input.setEnabled(true);
            updateSpinner.stopSpinning();
        }
    }
    
    void scrollBarMoved(ScrollBar* scrollBarThatHasMoved, double newRangeStart) override
    {
        repaint();
    }
    
    void paint(Graphics& g) override
    {
        PlugDataLook::paintStripes(g, 32, listBox.getHeight() + 24, *this, -1, listBox.getViewport()->getViewPositionY() + 4);
        
        if (errorMessage.isNotEmpty())
        {
            g.setColour(Colours::red);
            g.drawText(errorMessage, getLocalBounds().removeFromBottom(28).withTrimmedLeft(8).translated(0, 2), Justification::centredLeft);
        }
    }
    
    void paintOverChildren(Graphics& g) override
    {
        g.setFont(getLookAndFeel().getTextButtonFont(clearButton, 30));
        g.setColour(findColour(PlugDataColour::textColourId));
        
        g.drawText(Icons::Search, 0, 0, 30, 30, Justification::centred);
        
        if (input.getText().isEmpty())
        {
            g.setColour(findColour(PlugDataColour::toolbarOutlineColourId));
            g.setFont(Font());
            g.drawText("Type to search for objects or libraries", 32, 0, 350, 30, Justification::centredLeft);
        }
        
        g.setColour(findColour(PlugDataColour::toolbarOutlineColourId));
        g.drawLine(0, 28, getWidth(), 28);
    }
    
    int getNumRows() override
    {
        return searchResult.size() + packageManager->downloads.size();
    }
    
    void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
    {
    }
    
    Component* refreshComponentForRow(int rowNumber, bool isRowSelected, Component* existingComponentToUpdate) override
    {
        delete existingComponentToUpdate;
        
        
        if (isPositiveAndBelow(rowNumber, packageManager->downloads.size()))
        {
            return new DekenRowComponent(*this, packageManager->downloads[rowNumber]->packageInfo);
        }
        else if (isPositiveAndBelow(rowNumber - packageManager->downloads.size(), searchResult.size()))
        {
            return new DekenRowComponent(*this, searchResult.getReference(rowNumber - packageManager->downloads.size()));
        }
        
        return nullptr;
    }

    void filterResults()
    {
        String query = input.getText();
        
        PackageList newResult;
        
        searchResult.clear();
        
        // Show installed packages when query is empty
        if (query.isEmpty())
        {
            for (auto child : packageManager->packageState)
            {
                auto name = child.getType().toString();
                auto description = child.getProperty("Description").toString();
                auto timestamp = child.getProperty("Timestamp").toString();
                auto url = child.getProperty("URL").toString();
                auto version = child.getProperty("Version").toString();
                auto author = child.getProperty("Author").toString();
                auto objects = StringArray();
                
                auto info = PackageInfo(name, author, timestamp, url, description, version, objects);
                
                if (!packageManager->getDownloadForPackage(info))
                {
                    newResult.addIfNotAlreadyThere(info);
                }
            }
            
            searchResult = newResult;
            listBox.updateContent();
            updateSpinner.stopSpinning();
            
            return;
        }
        
        auto& allPackages = packageManager->allPackages;
        
        // First check for name match
        for(const auto& result : allPackages) {
            if(result.name.contains(query)) {
                newResult.addIfNotAlreadyThere(result);
            }
        }
        
        // Then check for description match
        for(const auto& result : allPackages) {
            if(result.description.contains(query)) {
                newResult.addIfNotAlreadyThere(result);
            }
        }
        
        // Then check for object match
        for(const auto& result : allPackages) {
            if(result.objects.contains(query)) {
                newResult.addIfNotAlreadyThere(result);
            }
        }
        
        // Then check for author match
        for(const auto& result : allPackages) {
            if(result.author.contains(query)) {
                newResult.addIfNotAlreadyThere(result);
            }
        }
        
        // Then check for object close match
        for(const auto& result : allPackages) {
            for(const auto& obj : result.objects) {
                if(obj.contains(query)) {
                    newResult.addIfNotAlreadyThere(result);
                }
            }
        }
        
        // Downloads are already always visible, so filter them out here
        newResult.removeIf([this](const PackageInfo& package){
            for(const auto* download : packageManager->downloads) {
                if(download->packageInfo == package) {
                    return true;
                }
            }
            return false;
        });
        
        searchResult = newResult;
        listBox.updateContent();
    }
    
    void resized() override
    {
        auto tableBounds = getLocalBounds().withTrimmedBottom(30);
        auto inputBounds = tableBounds.removeFromTop(28);
        
        const int statusbarHeight = 32;
        const int statusbarY = getHeight() - statusbarHeight;
        auto statusbarBounds = Rectangle<int>(2, statusbarY + 6, getWidth() - 6, statusbarHeight);
        
        input.setBounds(inputBounds);
        
        clearButton.setBounds(inputBounds.removeFromRight(30));
        updateSpinner.setBounds(inputBounds.removeFromRight(30));
        
        tableBounds.removeFromLeft(Sidebar::dragbarWidth);
        listBox.setBounds(tableBounds);
    
        refreshButton.setBounds(statusbarBounds.removeFromRight(statusbarHeight));
    }
    
    // Show error message in statusbar
    void showError(const String& message)
    {
        errorMessage = message;
        repaint();
    }
    
private:
    // List component to list packages
    ListBox listBox;
    
    // Last error message
    String errorMessage;
    
    // Current search result
    PackageList searchResult;
    
    TextButton refreshButton = TextButton(Icons::Refresh);

    PackageManager* packageManager = PackageManager::getInstance();
    
    TextEditor input;
    TextButton clearButton = TextButton(Icons::Clear);
    
    Spinner updateSpinner;
    
    // Component representing a search result
    // It holds package info about the package it represents
    // and can
    struct DekenRowComponent : public Component
    {
        Deken& deken;
        PackageInfo packageInfo;
        
        TextButton installButton = TextButton(Icons::SaveAs);
        TextButton reinstallButton = TextButton(Icons::Refresh);
        TextButton uninstallButton = TextButton(Icons::Clear);
        
        float installProgress;
        ValueTree packageState;
        
        DekenRowComponent(Deken& parent, PackageInfo& info) : deken(parent), packageInfo(info), packageState(deken.packageManager->packageState)
        {
            addChildComponent(installButton);
            addChildComponent(reinstallButton);
            addChildComponent(uninstallButton);
            
            installButton.setName("statusbar:install");
            reinstallButton.setName("statusbar:reinstall");
            uninstallButton.setName("statusbar:uninstall");
            
            uninstallButton.onClick = [this]()
            {
                setInstalled(false);
                deken.packageManager->uninstall(packageInfo);
                deken.filterResults();
            };
            
            reinstallButton.onClick = [this]()
            {
                auto* downloadTask = deken.packageManager->install(packageInfo);
                attachToDownload(downloadTask);
            };
            
            installButton.onClick = [this]()
            {
                auto* downloadTask = deken.packageManager->install(packageInfo);
                attachToDownload(downloadTask);
            };
            
            // Check if package is already installed
            setInstalled(deken.packageManager->packageExists(packageInfo));
            
            // Check if already in progress
            if (auto* task = deken.packageManager->getDownloadForPackage(packageInfo))
            {
                if(!task->isFinished) {
                    attachToDownload(task);
                }
            }
        }
        
        void attachToDownload(PackageManager::DownloadTask* task)
        {
            task->onProgress = [_this = SafePointer(this)](float progress)
            {
                if(!_this) return;
                _this->installProgress = progress;
                _this->repaint();
            };
            
            task->onFinish = [this, task](Result result)
            {
                task->isFinished = true;
                
                if(result.wasOk()) {
                    setInstalled(result);
                    deken.filterResults();
                }
                else {
                    deken.showError(result.getErrorMessage());
                    deken.filterResults();
                }

            };
            
            installButton.setVisible(false);
            reinstallButton.setVisible(false);
            uninstallButton.setVisible(false);
        }
        
        // Enables or disables buttons based on package state
        void setInstalled(bool installed)
        {
            installButton.setVisible(!installed);
            reinstallButton.setVisible(installed);
            uninstallButton.setVisible(installed);
            installProgress = 0.0f;
            
            repaint();
        }
        
        void paint(Graphics& g) override
        {
            g.setColour(findColour(ComboBox::textColourId));
            
            g.setFont(Font());
            g.drawFittedText(packageInfo.name, 5, 0, 200, getHeight(), Justification::centredLeft, 1, 0.8f);
            
            // draw progressbar
            if (deken.packageManager->getDownloadForPackage(packageInfo))
            {
                float width = getWidth() - 90.0f;
                float right = jmap(installProgress, 90.f, width);
                
                Path downloadPath;
                downloadPath.addLineSegment({90, 15, right, 15}, 1.0f);
                
                Path fullPath;
                fullPath.addLineSegment({90, 15, width, 15}, 1.0f);
                
                g.setColour(findColour(PlugDataColour::toolbarOutlineColourId));
                g.strokePath(fullPath, PathStrokeType(11.0f, PathStrokeType::JointStyle::curved, PathStrokeType::EndCapStyle::rounded));
                
                g.setColour(findColour(PlugDataColour::highlightColourId));
                g.strokePath(downloadPath, PathStrokeType(8.0f, PathStrokeType::JointStyle::curved, PathStrokeType::EndCapStyle::rounded));
            }
            else
            {
                g.drawFittedText(packageInfo.version, 90, 0, 150, getHeight(), Justification::centredLeft, 1, 0.8f);
                g.drawFittedText(packageInfo.author, 250, 0, 200, getHeight(), Justification::centredLeft, 1, 0.8f);
                g.drawFittedText(packageInfo.timestamp, 440, 0, 200, getHeight(), Justification::centredLeft, 1, 0.8f);
            }
        }
        
        void resized() override
        {
            installButton.setBounds(getWidth() - 40, 1, 26, 30);
            uninstallButton.setBounds(getWidth() - 40, 1, 26, 30);
            reinstallButton.setBounds(getWidth() - 70, 1, 26, 30);
        }
    };
};
