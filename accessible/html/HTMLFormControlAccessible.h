/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_A11Y_HTMLFormControlAccessible_H_
#define MOZILLA_A11Y_HTMLFormControlAccessible_H_

#include "FormControlAccessible.h"
#include "HyperTextAccessibleWrap.h"

namespace mozilla {
namespace a11y {

/**
 * Accessible for HTML progress element.
 */
typedef ProgressMeterAccessible<1> HTMLProgressMeterAccessible;

/**
 * Accessible for HTML input@type="checkbox".
 */
class HTMLCheckboxAccessible : public LeafAccessible
{

public:
  enum { eAction_Click = 0 };

  HTMLCheckboxAccessible(nsIContent* aContent, DocAccessible* aDoc) :
    LeafAccessible(aContent, aDoc)
  {
    // Ignore "CheckboxStateChange" DOM event in lieu of document observer
    // state change notification.
    mStateFlags |= eIgnoreDOMUIEvent;
  }

  // Accessible
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual uint64_t NativeState() MOZ_OVERRIDE;

  // ActionAccessible
  virtual uint8_t ActionCount() MOZ_OVERRIDE;
  virtual void ActionNameAt(uint8_t aIndex, nsAString& aName) MOZ_OVERRIDE;
  virtual bool DoAction(uint8_t aIndex) MOZ_OVERRIDE;

  // Widgets
  virtual bool IsWidget() const MOZ_OVERRIDE;
};


/**
 * Accessible for HTML input@type="radio" element.
 */
class HTMLRadioButtonAccessible : public RadioButtonAccessible
{

public:
  HTMLRadioButtonAccessible(nsIContent* aContent, DocAccessible* aDoc) :
    RadioButtonAccessible(aContent, aDoc)
  {
    // Ignore "RadioStateChange" DOM event in lieu of document observer
    // state change notification.
    mStateFlags |= eIgnoreDOMUIEvent;
  }

  // Accessible
  virtual uint64_t NativeState() MOZ_OVERRIDE;
  virtual void GetPositionAndSizeInternal(int32_t *aPosInSet,
                                          int32_t *aSetSize) MOZ_OVERRIDE;
};


/**
 * Accessible for HTML input@type="button", @type="submit", @type="image"
 * and HTML button elements.
 */
class HTMLButtonAccessible : public HyperTextAccessibleWrap
{

public:
  enum { eAction_Click = 0 };

  HTMLButtonAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual uint64_t State() MOZ_OVERRIDE;
  virtual uint64_t NativeState() MOZ_OVERRIDE;

  // ActionAccessible
  virtual uint8_t ActionCount() MOZ_OVERRIDE;
  virtual void ActionNameAt(uint8_t aIndex, nsAString& aName) MOZ_OVERRIDE;
  virtual bool DoAction(uint8_t aIndex) MOZ_OVERRIDE;

  // Widgets
  virtual bool IsWidget() const MOZ_OVERRIDE;

protected:
  // Accessible
  virtual ENameValueFlag NativeName(nsString& aName) MOZ_OVERRIDE;
};


/**
 * Accessible for HTML input@type="text", input@type="password", textarea and
 * other HTML text controls.
 */
class HTMLTextFieldAccessible MOZ_FINAL : public HyperTextAccessibleWrap
{

public:
  enum { eAction_Click = 0 };

  HTMLTextFieldAccessible(nsIContent* aContent, DocAccessible* aDoc);

  NS_DECL_ISUPPORTS_INHERITED

  // HyperTextAccessible
  virtual already_AddRefed<nsIEditor> GetEditor() const MOZ_OVERRIDE;

  // Accessible
  virtual void Value(nsString& aValue) MOZ_OVERRIDE;
  virtual void ApplyARIAState(uint64_t* aState) const MOZ_OVERRIDE;
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual uint64_t NativeState() MOZ_OVERRIDE;
  virtual already_AddRefed<nsIPersistentProperties> NativeAttributes() MOZ_OVERRIDE;

  // ActionAccessible
  virtual uint8_t ActionCount() MOZ_OVERRIDE;
  virtual void ActionNameAt(uint8_t aIndex, nsAString& aName) MOZ_OVERRIDE;
  virtual bool DoAction(uint8_t aIndex) MOZ_OVERRIDE;

  // Widgets
  virtual bool IsWidget() const MOZ_OVERRIDE;
  virtual Accessible* ContainerWidget() const MOZ_OVERRIDE;

protected:
  virtual ~HTMLTextFieldAccessible() {}

  // Accessible
  virtual ENameValueFlag NativeName(nsString& aName) MOZ_OVERRIDE;

  /**
   * Return a XUL widget element this input is part of.
   */
  nsIContent* XULWidgetElm() const { return mContent->GetBindingParent(); }
};


/**
 * Accessible for input@type="file" element.
 */
class HTMLFileInputAccessible : public HyperTextAccessibleWrap
{
public:
  HTMLFileInputAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual nsresult HandleAccEvent(AccEvent* aAccEvent) MOZ_OVERRIDE;
};


/**
 * Used for HTML input@type="number".
 */
class HTMLSpinnerAccessible : public AccessibleWrap
{
public:
  HTMLSpinnerAccessible(nsIContent* aContent, DocAccessible* aDoc) :
    AccessibleWrap(aContent, aDoc)
  {
    mStateFlags |= eHasNumericValue;
}

  // Accessible
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual void Value(nsString& aValue) MOZ_OVERRIDE;

  virtual double MaxValue() const MOZ_OVERRIDE;
  virtual double MinValue() const MOZ_OVERRIDE;
  virtual double CurValue() const MOZ_OVERRIDE;
  virtual double Step() const MOZ_OVERRIDE;
  virtual bool SetCurValue(double aValue) MOZ_OVERRIDE;
};


/**
  * Used for input@type="range" element.
  */
class HTMLRangeAccessible : public LeafAccessible
{
public:
  HTMLRangeAccessible(nsIContent* aContent, DocAccessible* aDoc) :
    LeafAccessible(aContent, aDoc)
  {
    mStateFlags |= eHasNumericValue;
  }

  // Accessible
  virtual void Value(nsString& aValue) MOZ_OVERRIDE;
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;

  // Value
  virtual double MaxValue() const MOZ_OVERRIDE;
  virtual double MinValue() const MOZ_OVERRIDE;
  virtual double CurValue() const MOZ_OVERRIDE;
  virtual double Step() const MOZ_OVERRIDE;
  virtual bool SetCurValue(double aValue) MOZ_OVERRIDE;

  // Widgets
  virtual bool IsWidget() const MOZ_OVERRIDE;
};


/**
 * Accessible for HTML fieldset element.
 */
class HTMLGroupboxAccessible : public HyperTextAccessibleWrap
{
public:
  HTMLGroupboxAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual mozilla::a11y::role NativeRole() MOZ_OVERRIDE;
  virtual Relation RelationByType(RelationType aType) MOZ_OVERRIDE;

protected:
  // Accessible
  virtual ENameValueFlag NativeName(nsString& aName) MOZ_OVERRIDE;

  // HTMLGroupboxAccessible
  nsIContent* GetLegend() const;
};


/**
 * Accessible for HTML legend element.
 */
class HTMLLegendAccessible : public HyperTextAccessibleWrap
{
public:
  HTMLLegendAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual Relation RelationByType(RelationType aType) MOZ_OVERRIDE;
};

/**
 * Accessible for HTML5 figure element.
 */
class HTMLFigureAccessible : public HyperTextAccessibleWrap
{
public:
  HTMLFigureAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual Relation RelationByType(RelationType aType) MOZ_OVERRIDE;

protected:
  // Accessible
  virtual ENameValueFlag NativeName(nsString& aName) MOZ_OVERRIDE;

  // HTMLLegendAccessible
  nsIContent* Caption() const;
};


/**
 * Accessible for HTML5 figcaption element.
 */
class HTMLFigcaptionAccessible : public HyperTextAccessibleWrap
{
public:
  HTMLFigcaptionAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // Accessible
  virtual Relation RelationByType(RelationType aType) MOZ_OVERRIDE;
};

} // namespace a11y
} // namespace mozilla

#endif
