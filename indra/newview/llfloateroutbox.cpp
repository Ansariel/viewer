/** 
 * @file llfloateroutbox.cpp
 * @brief Implementation of the merchant outbox window and of the marketplace listings window
 *
 * *TODO : Eventually, take out all the merchant outbox stuff and rename that file to llfloatermarketplacelistings
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfloateroutbox.h"

#include "llfloaterreg.h"
#include "llfolderview.h"
#include "llinventorybridge.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llinventorypanel.h"
#include "llmarketplacefunctions.h"
#include "llnotificationhandler.h"
#include "llnotificationmanager.h"
#include "llnotificationsutil.h"
#include "lltextbox.h"
#include "lltransientfloatermgr.h"
#include "lltrans.h"
#include "llviewernetwork.h"
#include "llwindowshade.h"


///----------------------------------------------------------------------------
/// LLOutboxNotification class
///----------------------------------------------------------------------------

LLNotificationsUI::LLOutboxNotification::LLOutboxNotification()
	: LLSystemNotificationHandler("Outbox", "outbox")
{
}

bool LLNotificationsUI::LLOutboxNotification::processNotification(const LLNotificationPtr& notify)
{
	LLFloaterOutbox* outbox_floater = LLFloaterReg::getTypedInstance<LLFloaterOutbox>("outbox");
	
	outbox_floater->showNotification(notify);

	return false;
}

void LLNotificationsUI::LLOutboxNotification::onDelete(LLNotificationPtr p)
{
	LLNotificationsUI::LLNotificationHandler * notification_handler = dynamic_cast<LLNotificationsUI::LLNotificationHandler*>(LLNotifications::instance().getChannel("AlertModal").get());
	if (notification_handler)
	{
		notification_handler->onDelete(p);
	}
}

///----------------------------------------------------------------------------
/// LLOutboxAddedObserver helper class
///----------------------------------------------------------------------------

class LLOutboxAddedObserver : public LLInventoryCategoryAddedObserver
{
public:
	LLOutboxAddedObserver(LLFloaterOutbox * outboxFloater)
		: LLInventoryCategoryAddedObserver()
		, mOutboxFloater(outboxFloater)
	{
	}
	
	void done()
	{
		for (cat_vec_t::iterator it = mAddedCategories.begin(); it != mAddedCategories.end(); ++it)
		{
			LLViewerInventoryCategory* added_category = *it;
			
			LLFolderType::EType added_category_type = added_category->getPreferredType();
			
			if (added_category_type == LLFolderType::FT_OUTBOX)
			{
				mOutboxFloater->initializeMarketPlace();
			}
		}
	}
	
private:
	LLFloaterOutbox *	mOutboxFloater;
};

///----------------------------------------------------------------------------
/// LLMarketplaceListingsAddedObserver helper class
///----------------------------------------------------------------------------

class LLMarketplaceListingsAddedObserver : public LLInventoryCategoryAddedObserver
{
public:
	LLMarketplaceListingsAddedObserver(LLFloaterMarketplaceListings * marketplace_listings_floater)
    : LLInventoryCategoryAddedObserver()
    , mMarketplaceListingsFloater(marketplace_listings_floater)
	{
	}
	
	void done()
	{
		for (cat_vec_t::iterator it = mAddedCategories.begin(); it != mAddedCategories.end(); ++it)
		{
			LLViewerInventoryCategory* added_category = *it;
			
			LLFolderType::EType added_category_type = added_category->getPreferredType();
			
			if (added_category_type == LLFolderType::FT_MARKETPLACE_LISTINGS)
			{
				mMarketplaceListingsFloater->initializeMarketPlace();
			}
		}
	}
	
private:
	LLFloaterMarketplaceListings *	mMarketplaceListingsFloater;
};


///----------------------------------------------------------------------------
/// LLFloaterOutbox
///----------------------------------------------------------------------------

LLFloaterOutbox::LLFloaterOutbox(const LLSD& key)
	: LLFloater(key)
	, mCategoriesObserver(NULL)
	, mCategoryAddedObserver(NULL)
	, mImportBusy(false)
	, mImportButton(NULL)
	, mInventoryFolderCountText(NULL)
	, mInventoryImportInProgress(NULL)
	, mInventoryPlaceholder(NULL)
	, mInventoryText(NULL)
	, mInventoryTitle(NULL)
	, mOutboxId(LLUUID::null)
	, mOutboxItemCount(0)
	, mOutboxTopLevelDropZone(NULL)
	, mWindowShade(NULL)
{
}

LLFloaterOutbox::~LLFloaterOutbox()
{
	if (mCategoriesObserver && gInventory.containsObserver(mCategoriesObserver))
	{
		gInventory.removeObserver(mCategoriesObserver);
	}
	delete mCategoriesObserver;
	
	if (mCategoryAddedObserver && gInventory.containsObserver(mCategoryAddedObserver))
	{
		gInventory.removeObserver(mCategoryAddedObserver);
	}
	delete mCategoryAddedObserver;
}

BOOL LLFloaterOutbox::postBuild()
{
	mInventoryFolderCountText = getChild<LLTextBox>("outbox_folder_count");
	mInventoryImportInProgress = getChild<LLView>("import_progress_indicator");
	mInventoryPlaceholder = getChild<LLView>("outbox_inventory_placeholder_panel");
	mInventoryText = mInventoryPlaceholder->getChild<LLTextBox>("outbox_inventory_placeholder_text");
	mInventoryTitle = mInventoryPlaceholder->getChild<LLTextBox>("outbox_inventory_placeholder_title");
	
	mImportButton = getChild<LLButton>("outbox_import_btn");
	mImportButton->setCommitCallback(boost::bind(&LLFloaterOutbox::onImportButtonClicked, this));

	mOutboxTopLevelDropZone = getChild<LLPanel>("outbox_generic_drag_target");

	LLFocusableElement::setFocusReceivedCallback(boost::bind(&LLFloaterOutbox::onFocusReceived, this));

	// Observe category creation to catch outbox creation (moot if already existing)
	mCategoryAddedObserver = new LLOutboxAddedObserver(this);
	gInventory.addObserver(mCategoryAddedObserver);
	
	return TRUE;
}

void LLFloaterOutbox::cleanOutbox()
{
    // Note: we cannot delete the mOutboxInventoryPanel as that point
    // as this is called through callback observers of the panel itself.
    // Doing so would crash rapidly.
	
	// Invalidate the outbox data
    mOutboxId.setNull();
    mOutboxItemCount = 0;
}

void LLFloaterOutbox::onClose(bool app_quitting)
{
	if (mWindowShade)
	{
		delete mWindowShade;

		mWindowShade = NULL;
	}
}

void LLFloaterOutbox::onOpen(const LLSD& key)
{
	//
	// Initialize the Market Place or go update the outbox
	//
	if (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() == MarketplaceStatusCodes::MARKET_PLACE_NOT_INITIALIZED)
	{
		initializeMarketPlace();
	}
	else 
	{
		setupOutbox();
	}
	
	//
	// Update the floater view
	//
	updateView();
	
	//
	// Trigger fetch of outbox contents
	//
	fetchOutboxContents();
}

void LLFloaterOutbox::onFocusReceived()
{
	fetchOutboxContents();
}

void LLFloaterOutbox::fetchOutboxContents()
{
	if (mOutboxId.notNull())
	{
		LLInventoryModelBackgroundFetch::instance().start(mOutboxId);
	}
}

void LLFloaterOutbox::setupOutbox()
{
	if (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() != MarketplaceStatusCodes::MARKET_PLACE_MERCHANT)
	{
		// If we are *not* a merchant or we have no market place connection established yet, do nothing
		return;
	}
		
	// We are a merchant. Get the outbox, create it if needs be.
	LLUUID outbox_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_OUTBOX, true);
	if (outbox_id.isNull())
	{
		// We should never get there unless the inventory fails badly
		llerrs << "Inventory problem: failure to create the outbox for a merchant!" << llendl;
		return;
	}
    
    // Consolidate Merchant Outbox
    // We shouldn't have to do that but with a client/server system relying on a "well known folder" convention, things get messy and conventions get broken down eventually
    gInventory.consolidateForType(outbox_id, LLFolderType::FT_OUTBOX);
    
    if (outbox_id == mOutboxId)
    {
        llwarns << "Inventory warning: Merchant outbox already set" << llendl;
        return;
    }
    mOutboxId = outbox_id;

	// No longer need to observe new category creation
	if (mCategoryAddedObserver && gInventory.containsObserver(mCategoryAddedObserver))
	{
		gInventory.removeObserver(mCategoryAddedObserver);
		delete mCategoryAddedObserver;
		mCategoryAddedObserver = NULL;
	}
	llassert(!mCategoryAddedObserver);
	
	// Create observer for outbox modifications : clear the old one and create a new one
	if (mCategoriesObserver && gInventory.containsObserver(mCategoriesObserver))
	{
		gInventory.removeObserver(mCategoriesObserver);
		delete mCategoriesObserver;
	}
    mCategoriesObserver = new LLInventoryCategoriesObserver();
    gInventory.addObserver(mCategoriesObserver);
    mCategoriesObserver->addCategory(mOutboxId, boost::bind(&LLFloaterOutbox::onOutboxChanged, this));
	llassert(mCategoriesObserver);
	
	// Set up the outbox inventory view
	LLInventoryPanel* inventory_panel = mOutboxInventoryPanel.get();
    if (inventory_panel)
    {
        delete inventory_panel;
    }
    inventory_panel = LLUICtrlFactory::createFromFile<LLInventoryPanel>("panel_outbox_inventory.xml", mInventoryPlaceholder->getParent(), LLInventoryPanel::child_registry_t::instance());
    mOutboxInventoryPanel = inventory_panel->getInventoryPanelHandle();
	llassert(mOutboxInventoryPanel.get() != NULL);
	
	// Reshape the inventory to the proper size
	LLRect inventory_placeholder_rect = mInventoryPlaceholder->getRect();
	inventory_panel->setShape(inventory_placeholder_rect);
	
	// Set the sort order newest to oldest
	inventory_panel->getFolderViewModel()->setSorter(LLInventoryFilter::SO_FOLDERS_BY_NAME);	
	inventory_panel->getFilter().markDefault();
    
	// Get the content of the outbox
	fetchOutboxContents();
}

void LLFloaterOutbox::initializeMarketPlace()
{
	//
	// Initialize the marketplace import API
	//
	LLMarketplaceInventoryImporter& importer = LLMarketplaceInventoryImporter::instance();
	
    if (!importer.isInitialized())
    {
        importer.setInitializationErrorCallback(boost::bind(&LLFloaterOutbox::initializationReportError, this, _1, _2));
        importer.setStatusChangedCallback(boost::bind(&LLFloaterOutbox::importStatusChanged, this, _1));
        importer.setStatusReportCallback(boost::bind(&LLFloaterOutbox::importReportResults, this, _1, _2));
        importer.initialize();
    }
}

void LLFloaterOutbox::setStatusString(const std::string& statusString)
{
	llassert(mInventoryFolderCountText != NULL);

	mInventoryFolderCountText->setText(statusString);
}

void LLFloaterOutbox::updateFolderCount()
{
	if (mOutboxInventoryPanel.get() && mOutboxId.notNull())
	{
        S32 item_count = 0;

        if (mOutboxId.notNull())
        {
            LLInventoryModel::cat_array_t * cats;
            LLInventoryModel::item_array_t * items;
            gInventory.getDirectDescendentsOf(mOutboxId, cats, items);

            item_count = cats->count() + items->count();
        }
	
        mOutboxItemCount = item_count;
    }
    else
    {
        // If there's no outbox, the number of items in it should be set to 0 for consistency
        mOutboxItemCount = 0;
    }

	if (!mImportBusy)
	{
		updateFolderCountStatus();
	}
}

void LLFloaterOutbox::updateFolderCountStatus()
{
	if (mOutboxInventoryPanel.get() && mOutboxId.notNull())
	{
		switch (mOutboxItemCount)
		{
			case 0:	setStatusString(getString("OutboxFolderCount0"));	break;
			case 1:	setStatusString(getString("OutboxFolderCount1"));	break;
			default:
			{
				std::string item_count_str = llformat("%d", mOutboxItemCount);
				
				LLStringUtil::format_map_t args;
				args["[NUM]"] = item_count_str;
				
				setStatusString(getString("OutboxFolderCountN", args));
				break;
			}
		}
	}

	mImportButton->setEnabled(mOutboxItemCount > 0);
}

void LLFloaterOutbox::updateView()
{
	updateFolderCount();
	LLInventoryPanel* panel = mOutboxInventoryPanel.get();

	if (mOutboxItemCount > 0)
	{
		panel->setVisible(TRUE);
		mInventoryPlaceholder->setVisible(FALSE);
		mOutboxTopLevelDropZone->setVisible(TRUE);
	}
	else
	{
		if (panel)
		{
			panel->setVisible(FALSE);
		}
		
		// Show the drop zone if there is an outbox folder
		mOutboxTopLevelDropZone->setVisible(mOutboxId.notNull());

		mInventoryPlaceholder->setVisible(TRUE);

		std::string outbox_text;
		std::string outbox_title;
		std::string outbox_tooltip;
		
		const LLSD& subs = getMarketplaceStringSubstitutions();
		U32 mkt_status = LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus();
		
		if (mOutboxId.notNull())
		{
            // Does the outbox needs recreation?
            if ((mOutboxInventoryPanel.get() == NULL) || !gInventory.getCategory(mOutboxId))
            {
                setupOutbox();
            }
			// "Outbox is empty!" message strings
			outbox_text = LLTrans::getString("InventoryOutboxNoItems", subs);
			outbox_title = LLTrans::getString("InventoryOutboxNoItemsTitle");
			outbox_tooltip = LLTrans::getString("InventoryOutboxNoItemsTooltip");
		}
		else if (mkt_status <= MarketplaceStatusCodes::MARKET_PLACE_INITIALIZING)
		{
			// "Initializing!" message strings
			outbox_text = LLTrans::getString("InventoryOutboxInitializing", subs);
			outbox_title = LLTrans::getString("InventoryOutboxInitializingTitle");
			outbox_tooltip = LLTrans::getString("InventoryOutboxInitializingTooltip");
		}
		else if (mkt_status == MarketplaceStatusCodes::MARKET_PLACE_NOT_MERCHANT)
		{
			// "Not a merchant!" message strings
			outbox_text = LLTrans::getString("InventoryOutboxNotMerchant", subs);
			outbox_title = LLTrans::getString("InventoryOutboxNotMerchantTitle");
			outbox_tooltip = LLTrans::getString("InventoryOutboxNotMerchantTooltip");
		}
		else
		{
			// "Errors!" message strings
			outbox_text = LLTrans::getString("InventoryOutboxError", subs);
			outbox_title = LLTrans::getString("InventoryOutboxErrorTitle");
			outbox_tooltip = LLTrans::getString("InventoryOutboxErrorTooltip");
		}
		
		mInventoryText->setValue(outbox_text);
		mInventoryTitle->setValue(outbox_title);
		mInventoryPlaceholder->getParent()->setToolTip(outbox_tooltip);
	}
}

bool isAccepted(EAcceptance accept)
{
	return (accept >= ACCEPT_YES_COPY_SINGLE);
}

BOOL LLFloaterOutbox::handleDragAndDrop(S32 x, S32 y, MASK mask, BOOL drop,
										EDragAndDropType cargo_type,
										void* cargo_data,
										EAcceptance* accept,
										std::string& tooltip_msg)
{
	if ((mOutboxInventoryPanel.get() == NULL) ||
		(mWindowShade && mWindowShade->isShown()) ||
		LLMarketplaceInventoryImporter::getInstance()->isImportInProgress() ||
        mOutboxId.isNull())
	{
		return FALSE;
	}
	
	LLView * handled_view = childrenHandleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
	BOOL handled = (handled_view != NULL);

	// Determine if the mouse is inside the inventory panel itself or just within the floater
	bool pointInInventoryPanel = false;
	bool pointInInventoryPanelChild = false;
	LLInventoryPanel* panel = mOutboxInventoryPanel.get();
	LLFolderView* root_folder = panel->getRootFolder();
	if (panel->getVisible())
	{
		S32 inv_x, inv_y;
		localPointToOtherView(x, y, &inv_x, &inv_y, panel);

		pointInInventoryPanel = panel->getRect().pointInRect(inv_x, inv_y);

		LLView * inventory_panel_child_at_point = panel->childFromPoint(inv_x, inv_y, true);
		pointInInventoryPanelChild = (inventory_panel_child_at_point != root_folder);
	}
	
	// Pass all drag and drop for this floater to the outbox inventory control
	if (!handled || !isAccepted(*accept))
	{
		// Handle the drag and drop directly to the root of the outbox if we're not in the inventory panel
		// (otherwise the inventory panel itself will handle the drag and drop operation, without any override)
		if (!pointInInventoryPanel)
		{
			handled = root_folder->handleDragAndDropToThisFolder(mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
		}

		mOutboxTopLevelDropZone->setBackgroundVisible(handled && !drop && isAccepted(*accept));
	}
	else
	{
		mOutboxTopLevelDropZone->setBackgroundVisible(!pointInInventoryPanelChild);
	}
	
	return handled;
}

BOOL LLFloaterOutbox::handleHover(S32 x, S32 y, MASK mask)
{
	mOutboxTopLevelDropZone->setBackgroundVisible(FALSE);

	return LLFloater::handleHover(x, y, mask);
}

void LLFloaterOutbox::onMouseLeave(S32 x, S32 y, MASK mask)
{
	mOutboxTopLevelDropZone->setBackgroundVisible(FALSE);

	LLFloater::onMouseLeave(x, y, mask);
}

void LLFloaterOutbox::onImportButtonClicked()
{
    if (mOutboxInventoryPanel.get())
    {
        mOutboxInventoryPanel.get()->clearSelection();
    }

	mImportBusy = LLMarketplaceInventoryImporter::instance().triggerImport();
}

void LLFloaterOutbox::onOutboxChanged()
{
    LLViewerInventoryCategory* category = gInventory.getCategory(mOutboxId);
	if (mOutboxId.notNull() && category)
    {
        fetchOutboxContents();
        updateView();
    }
    else
    {
        cleanOutbox();
    }
}

void LLFloaterOutbox::importReportResults(U32 status, const LLSD& content)
{
	if (status == MarketplaceErrorCodes::IMPORT_DONE)
	{
		LLNotificationsUtil::add("OutboxImportComplete");
	}
	else if (status == MarketplaceErrorCodes::IMPORT_DONE_WITH_ERRORS)
	{
		const LLSD& subs = getMarketplaceStringSubstitutions();

		LLNotificationsUtil::add("OutboxImportHadErrors", subs);
	}
	else
	{
		char status_string[16];
		sprintf(status_string, "%d", status);
		
		LLSD subs;
		subs["[ERROR_CODE]"] = status_string;
		
		LLNotificationsUtil::add("OutboxImportFailed", subs);
	}
	
	updateView();
}

void LLFloaterOutbox::importStatusChanged(bool inProgress)
{
	if (mOutboxId.isNull() && (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() == MarketplaceStatusCodes::MARKET_PLACE_MERCHANT))
	{
		setupOutbox();
	}
	
	if (inProgress)
	{
		if (mImportBusy)
		{
			setStatusString(getString("OutboxImporting"));
		}
		else
		{
			setStatusString(getString("OutboxInitializing"));
		}
		
		mImportBusy = true;
		mImportButton->setEnabled(false);
		mInventoryImportInProgress->setVisible(true);
	}
	else
	{
		setStatusString("");
		mImportBusy = false;
		mImportButton->setEnabled(mOutboxItemCount > 0);
		mInventoryImportInProgress->setVisible(false);
	}
	
	updateView();
}

void LLFloaterOutbox::initializationReportError(U32 status, const LLSD& content)
{
	if (status >= MarketplaceErrorCodes::IMPORT_BAD_REQUEST)
	{
		char status_string[16];
		sprintf(status_string, "%d", status);
		
		LLSD subs;
		subs["[ERROR_CODE]"] = status_string;
		
		LLNotificationsUtil::add("OutboxInitFailed", subs);
	}
	
	updateView();
}

void LLFloaterOutbox::showNotification(const LLNotificationPtr& notification)
{
	LLNotificationsUI::LLNotificationHandler * notification_handler = dynamic_cast<LLNotificationsUI::LLNotificationHandler*>(LLNotifications::instance().getChannel("AlertModal").get());
	llassert(notification_handler);
	
	notification_handler->processNotification(notification);
}

///----------------------------------------------------------------------------
/// LLFloaterMarketplaceListings
///----------------------------------------------------------------------------

LLFloaterMarketplaceListings::LLFloaterMarketplaceListings(const LLSD& key)
: LLFloater(key)
, mCategoriesObserver(NULL)
, mCategoryAddedObserver(NULL)
, mRootFolderId(LLUUID::null)
, mInventoryStatus(NULL)
, mInventoryInitializationInProgress(NULL)
, mInventoryPlaceholder(NULL)
, mInventoryText(NULL)
, mInventoryTitle(NULL)
{
	mCommitCallbackRegistrar.add("Marketplace.ViewSort.Action",  boost::bind(&LLFloaterMarketplaceListings::onViewSortMenuItemClicked,  this, _2));
	mEnableCallbackRegistrar.add("Marketplace.ViewSort.CheckItem",	boost::bind(&LLFloaterMarketplaceListings::onViewSortMenuItemCheck,	this, _2));
}

LLFloaterMarketplaceListings::~LLFloaterMarketplaceListings()
{
	if (mCategoriesObserver && gInventory.containsObserver(mCategoriesObserver))
	{
		gInventory.removeObserver(mCategoriesObserver);
	}
	delete mCategoriesObserver;
	
	if (mCategoryAddedObserver && gInventory.containsObserver(mCategoryAddedObserver))
	{
		gInventory.removeObserver(mCategoryAddedObserver);
	}
	delete mCategoryAddedObserver;
}

BOOL LLFloaterMarketplaceListings::postBuild()
{
	mInventoryStatus = getChild<LLTextBox>("marketplace_status");
	mInventoryInitializationInProgress = getChild<LLView>("initialization_progress_indicator");
	mInventoryPlaceholder = getChild<LLView>("marketplace_listings_inventory_placeholder_panel");
	mInventoryText = mInventoryPlaceholder->getChild<LLTextBox>("marketplace_listings_inventory_placeholder_text");
	mInventoryTitle = mInventoryPlaceholder->getChild<LLTextBox>("marketplace_listings_inventory_placeholder_title");

	LLFocusableElement::setFocusReceivedCallback(boost::bind(&LLFloaterMarketplaceListings::onFocusReceived, this));
    
	// Observe category creation to catch marketplace listings creation (moot if already existing)
	mCategoryAddedObserver = new LLMarketplaceListingsAddedObserver(this);
	gInventory.addObserver(mCategoryAddedObserver);
	
    // Merov : Debug : fetch aggressively so we can create test data right onOpen()
    llinfos << "Merov : postBuild, do fetchContent() ahead of time" << llendl;
	fetchContents();

	return TRUE;
}

void LLFloaterMarketplaceListings::clean()
{
    // Note: we cannot delete the mOutboxInventoryPanel as that point
    // as this is called through callback observers of the panel itself.
    // Doing so would crash rapidly.
	
	// Invalidate the outbox data
    mRootFolderId.setNull();
}

void LLFloaterMarketplaceListings::onClose(bool app_quitting)
{
}

void LLFloaterMarketplaceListings::onOpen(const LLSD& key)
{
	//
	// Initialize the Market Place or go update the outbox
	//
	if (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() == MarketplaceStatusCodes::MARKET_PLACE_NOT_INITIALIZED)
	{
		initializeMarketPlace();
	}
	else
	{
		setup();
	}
	
    // Merov : Debug : Create fake Marketplace data if none is present
	if (LLMarketplaceData::instance().isEmpty() && (getFolderCount() > 0))
	{
        LLInventoryModel::cat_array_t* cats;
        LLInventoryModel::item_array_t* items;
        gInventory.getDirectDescendentsOf(mRootFolderId, cats, items);
        
        int index = 0;
        for (LLInventoryModel::cat_array_t::iterator iter = cats->begin(); iter != cats->end(); iter++, index++)
        {
            LLViewerInventoryCategory* category = *iter;
            if (index%3)
            {
                LLMarketplaceData::instance().addTestItem(category->getUUID());
                if (index%3 == 1)
                {
                    LLMarketplaceData::instance().setListingID(category->getUUID(),"TestingID1234");
                }
                LLMarketplaceData::instance().setActivation(category->getUUID(),(index%2));
            }
        }
    }

	//
	// Update the floater view
	//
	updateView();
	
	//
	// Trigger fetch of the contents
	//
	fetchContents();
}

void LLFloaterMarketplaceListings::onFocusReceived()
{
	fetchContents();
}


void LLFloaterMarketplaceListings::onViewSortMenuItemClicked(const LLSD& userdata)
{
    /*
	std::string chosen_item = userdata.asString();
    
	if (chosen_item == "sort_by_stock_amount")
	{
		setSortOrder(mNearbyList, E_SORT_BY_RECENT_SPEAKERS);
	}
	else if (chosen_item == "show_low_stock")
	{
		mNearbyList->toggleIcons();
	}
     */
}

bool LLFloaterMarketplaceListings::onViewSortMenuItemCheck(const LLSD& userdata)
{
    /*
	std::string item = userdata.asString();
	U32 sort_order = gSavedSettings.getU32("NearbyPeopleSortOrder");
    
	if (item == "sort_by_recent_speakers")
		return sort_order == E_SORT_BY_RECENT_SPEAKERS;
	if (item == "sort_name")
		return sort_order == E_SORT_BY_NAME;
	if (item == "sort_distance")
		return sort_order == E_SORT_BY_DISTANCE;
    */
	return false;
}


void LLFloaterMarketplaceListings::fetchContents()
{
	if (mRootFolderId.notNull())
	{
		LLInventoryModelBackgroundFetch::instance().start(mRootFolderId);
	}
}

void LLFloaterMarketplaceListings::setup()
{
	if (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() != MarketplaceStatusCodes::MARKET_PLACE_MERCHANT)
	{
		// If we are *not* a merchant or we have no market place connection established yet, do nothing
		return;
	}
    
	// We are a merchant. Get the Marketplace listings folder, create it if needs be.
	LLUUID outbox_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS, true);
	if (outbox_id.isNull())
	{
		// We should never get there unless the inventory fails badly
		llinfos << "Merov : Inventory problem: failure to create the marketplace listings folder for a merchant!" << llendl;
		llerrs << "Inventory problem: failure to create the marketplace listings folder for a merchant!" << llendl;
		return;
	}
    
    // Consolidate Marketplace listings
    // We shouldn't have to do that but with a client/server system relying on a "well known folder" convention, things get messy and conventions get broken down eventually
    gInventory.consolidateForType(outbox_id, LLFolderType::FT_MARKETPLACE_LISTINGS);
    
    if (outbox_id == mRootFolderId)
    {
        llinfos << "Merov : Inventory warning: Marketplace listings folder already set" << llendl;
        llwarns << "Inventory warning: Marketplace listings folder already set" << llendl;
        return;
    }
    mRootFolderId = outbox_id;
    
	// No longer need to observe new category creation
	if (mCategoryAddedObserver && gInventory.containsObserver(mCategoryAddedObserver))
	{
		gInventory.removeObserver(mCategoryAddedObserver);
		delete mCategoryAddedObserver;
		mCategoryAddedObserver = NULL;
	}
	llassert(!mCategoryAddedObserver);
	
	// Create observer for marketplace listings modifications : clear the old one and create a new one
	if (mCategoriesObserver && gInventory.containsObserver(mCategoriesObserver))
	{
		gInventory.removeObserver(mCategoriesObserver);
		delete mCategoriesObserver;
	}
    mCategoriesObserver = new LLInventoryCategoriesObserver();
    gInventory.addObserver(mCategoriesObserver);
    mCategoriesObserver->addCategory(mRootFolderId, boost::bind(&LLFloaterMarketplaceListings::onChanged, this));
	llassert(mCategoriesObserver);
	
	// Set up the marketplace listings inventory view
    LLPanel* inventory_panel = LLUICtrlFactory::createFromFile<LLPanel>("panel_marketplace_listings_inventory.xml", mInventoryPlaceholder->getParent(), LLInventoryPanel::child_registry_t::instance());
	LLInventoryPanel* items_panel = inventory_panel->getChild<LLInventoryPanel>("All Items");
    mInventoryPanel = items_panel->getInventoryPanelHandle();
	llassert(mInventoryPanel.get() != NULL);
	
	// Reshape the inventory to the proper size
	LLRect inventory_placeholder_rect = mInventoryPlaceholder->getRect();
	inventory_panel->setShape(inventory_placeholder_rect);
	
	// Set the sort order newest to oldest
	items_panel->getFolderViewModel()->setSorter(LLInventoryFilter::SO_FOLDERS_BY_NAME);
	items_panel->getFilter().markDefault();
    
    // Set filters on the 3 prefiltered panels
	items_panel = inventory_panel->getChild<LLInventoryPanel>("Active Items");
	items_panel->getFolderViewModel()->setSorter(LLInventoryFilter::SO_FOLDERS_BY_NAME);
	items_panel->getFilter().setFilterMarketplaceActiveFolders();
	items_panel->getFilter().markDefault();
	items_panel = inventory_panel->getChild<LLInventoryPanel>("Inactive Items");
	items_panel->getFolderViewModel()->setSorter(LLInventoryFilter::SO_FOLDERS_BY_NAME);
	items_panel->getFilter().setFilterMarketplaceInactiveFolders();
	items_panel->getFilter().markDefault();
	items_panel = inventory_panel->getChild<LLInventoryPanel>("Unassociated Items");
	items_panel->getFolderViewModel()->setSorter(LLInventoryFilter::SO_FOLDERS_BY_NAME);
	items_panel->getFilter().setFilterMarketplaceUnassociatedFolders();
	items_panel->getFilter().markDefault();

	// Get the content of the marketplace listings folder
	fetchContents();
}

void LLFloaterMarketplaceListings::initializeMarketPlace()
{
	//
	// Initialize the marketplace import API
	//
	LLMarketplaceInventoryImporter& importer = LLMarketplaceInventoryImporter::instance();
	
    if (!importer.isInitialized())
    {
        importer.setInitializationErrorCallback(boost::bind(&LLFloaterMarketplaceListings::initializationReportError, this, _1, _2));
        importer.setStatusChangedCallback(boost::bind(&LLFloaterMarketplaceListings::importStatusChanged, this, _1));
        importer.setStatusReportCallback(boost::bind(&LLFloaterMarketplaceListings::importReportResults, this, _1, _2));
        importer.initialize();
    }
}

S32 LLFloaterMarketplaceListings::getFolderCount()
{
	if (mInventoryPanel.get() && mRootFolderId.notNull())
	{
        LLInventoryModel::cat_array_t * cats;
        LLInventoryModel::item_array_t * items;
        gInventory.getDirectDescendentsOf(mRootFolderId, cats, items);
            
        return (cats->count() + items->count());
    }
    else
    {
        return 0;
    }
}

void LLFloaterMarketplaceListings::setStatusString(const std::string& statusString)
{
	mInventoryStatus->setText(statusString);
}

void LLFloaterMarketplaceListings::updateView()
{
	LLInventoryPanel* panel = mInventoryPanel.get();
    
	if (getFolderCount() > 0)
	{
		panel->setVisible(TRUE);
		mInventoryPlaceholder->setVisible(FALSE);
	}
	else
	{
        if (panel)
        {
            panel->setVisible(FALSE);
        }
		
        std::string text;
        std::string title;
        std::string tooltip;
    
        const LLSD& subs = getMarketplaceStringSubstitutions();
        U32 mkt_status = LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus();
    
        // *TODO : check those messages and create better appropriate ones in strings.xml
        if (mRootFolderId.notNull())
        {
            // Does the outbox needs recreation?
            if ((panel == NULL) || !gInventory.getCategory(mRootFolderId))
            {
                setup();
            }
            // "Marketplace listings is empty!" message strings
            text = LLTrans::getString("InventoryMarketplaceListingsNoItems", subs);
            title = LLTrans::getString("InventoryMarketplaceListingsNoItemsTitle");
            tooltip = LLTrans::getString("InventoryMarketplaceListingsNoItemsTooltip");
        }
        else if (mkt_status <= MarketplaceStatusCodes::MARKET_PLACE_INITIALIZING)
        {
            // "Initializing!" message strings
            text = LLTrans::getString("InventoryOutboxInitializing", subs);
            title = LLTrans::getString("InventoryOutboxInitializingTitle");
            tooltip = LLTrans::getString("InventoryOutboxInitializingTooltip");
        }
        else if (mkt_status == MarketplaceStatusCodes::MARKET_PLACE_NOT_MERCHANT)
        {
            // "Not a merchant!" message strings
            text = LLTrans::getString("InventoryOutboxNotMerchant", subs);
            title = LLTrans::getString("InventoryOutboxNotMerchantTitle");
            tooltip = LLTrans::getString("InventoryOutboxNotMerchantTooltip");
        }
        else
        {
            // "Errors!" message strings
            text = LLTrans::getString("InventoryOutboxError", subs);
            title = LLTrans::getString("InventoryOutboxErrorTitle");
            tooltip = LLTrans::getString("InventoryOutboxErrorTooltip");
        }
    
        mInventoryText->setValue(text);
        mInventoryTitle->setValue(title);
        mInventoryPlaceholder->getParent()->setToolTip(tooltip);
    }
}

bool LLFloaterMarketplaceListings::isAccepted(EAcceptance accept)
{
    // *TODO : Need a bit more test on what we accept: depends of what and where...
	return (accept >= ACCEPT_YES_COPY_SINGLE);
}


BOOL LLFloaterMarketplaceListings::handleDragAndDrop(S32 x, S32 y, MASK mask, BOOL drop,
										EDragAndDropType cargo_type,
										void* cargo_data,
										EAcceptance* accept,
										std::string& tooltip_msg)
{
	if ((mInventoryPanel.get() == NULL) || mRootFolderId.isNull())
	{
		return FALSE;
	}
	
	LLView * handled_view = childrenHandleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
	BOOL handled = (handled_view != NULL);
    
	// Determine if the mouse is inside the inventory panel itself or just within the floater
	bool pointInInventoryPanel = false;
	bool pointInInventoryPanelChild = false;
	LLInventoryPanel* panel = mInventoryPanel.get();
	LLFolderView* root_folder = panel->getRootFolder();
	if (panel->getVisible())
	{
		S32 inv_x, inv_y;
		localPointToOtherView(x, y, &inv_x, &inv_y, panel);
        
		pointInInventoryPanel = panel->getRect().pointInRect(inv_x, inv_y);
        
		LLView * inventory_panel_child_at_point = panel->childFromPoint(inv_x, inv_y, true);
		pointInInventoryPanelChild = (inventory_panel_child_at_point != root_folder);
	}
	
	// Pass all drag and drop for this floater to the outbox inventory control
	if (!handled || !isAccepted(*accept))
	{
		// Handle the drag and drop directly to the root of the outbox if we're not in the inventory panel
		// (otherwise the inventory panel itself will handle the drag and drop operation, without any override)
		if (!pointInInventoryPanel)
		{
			handled = root_folder->handleDragAndDropToThisFolder(mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
		}
	}
	
	return handled;
}

BOOL LLFloaterMarketplaceListings::handleHover(S32 x, S32 y, MASK mask)
{
	return LLFloater::handleHover(x, y, mask);
}

void LLFloaterMarketplaceListings::onMouseLeave(S32 x, S32 y, MASK mask)
{
	LLFloater::onMouseLeave(x, y, mask);
}

void LLFloaterMarketplaceListings::onChanged()
{
    LLViewerInventoryCategory* category = gInventory.getCategory(mRootFolderId);
	if (mRootFolderId.notNull() && category)
    {
        fetchContents();
        updateView();
    }
    else
    {
        clean();
    }
}

void LLFloaterMarketplaceListings::initializationReportError(U32 status, const LLSD& content)
{
	updateView();
}

void LLFloaterMarketplaceListings::importStatusChanged(bool inProgress)
{
	if (mRootFolderId.isNull() && (LLMarketplaceInventoryImporter::getInstance()->getMarketPlaceStatus() == MarketplaceStatusCodes::MARKET_PLACE_MERCHANT))
	{
		setup();
	}

	if (inProgress)
	{
        setStatusString(getString("MarketplaceListingsInitializing"));
		mInventoryInitializationInProgress->setVisible(true);
	}
	else
	{
		setStatusString("");
		mInventoryInitializationInProgress->setVisible(false);
	}
	
	updateView();
}

void LLFloaterMarketplaceListings::importReportResults(U32 status, const LLSD& content)
{	
	updateView();
}


